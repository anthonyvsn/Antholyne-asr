/*
 * Déroulement :
 *   1. le PC envoie un CMD_MSG (id_src = 0) à une carte -> elle devient MASTER
 *   2. le master tire une clé 16 bits au hasard et envoie CMD_INIT (payload = clé) en aval
 *   3. chaque slave répond ACK à l'expéditeur, garde la clé et propage l'INIT en aval
 *   4. un slave sans réponse en aval (ou qui reçoit un ACK avec id_src = 0, donc du PC)
 *      sait qu'il est le dernier de la chaîne
 *   5. les messages sont chiffrés (XOR avec la clé) par le master, relayés par les slaves,
 *      et déchiffrés par le dernier qui les renvoie en clair au PC
 *
 * Traçabilité exigences (exigence.md) -> code. Chaque bloc du code est marqué [EX-xx].
 *
 *   GÉNÉRAL
 *   EX-01  Émettre chaque trame au format header + payload + CRC         -> envoyer_trame()
 *   EX-02  Ignorer les octets reçus tant que le start_byte n'est pas vu  -> lire_port(), RX_ATTENTE_START
 *   EX-03  Rôle master à la carte reliée au PC, slave aux autres         -> sur_msg() (PC), sur_init() (amont)
 *   MASTER
 *   EX-04  Le master reçoit un message clair avec l'id 0x0 venant du PC  -> sur_msg(), branche PORT_PC
 *   EX-05  Générer aléatoirement une clé de 16 bits                      -> generer_cle()
 *   EX-06  Envoyer un INIT (0x00) avec la clé 16 bits dans le payload    -> demarrer_synchro()
 *   EX-07  Sur un INIT, envoyer un ACK à l'expéditeur                    -> sur_init()
 *   SLAVES
 *   EX-08  Le slave propage l'INIT aux autres slaves                     -> sur_init() -> demarrer_synchro()
 *   EX-09  Vérifier le CRC à la réception                                -> traiter_trame()
 *   EX-10  Détecter si le nœud est le dernier (ACK avec id_src NULL = PC)-> sur_ack()
 *          [COMPLÉMENT] ou aucune réponse à l'INIT après 3 retransmissions -> retransmettre()
 *          (le PC est sur l'USART2 du dernier, son UART4 n'est relié à rien)
 *   EX-11  Le dernier déchiffre avec la clé et transmet en clair au PC   -> envoyer_fifo(), xor_cle()
 *   ERREUR
 *   EX-12  CRC invalide : rejet + CMD_ERROR à l'émetteur                 -> traiter_trame()
 *   EX-13  length > 16 : rejet + CMD_ERROR                               -> lire_port(), RX_HEADER
 *   EX-14  Commande inconnue : rejet + CMD_ERROR                         -> traiter_trame(), default
 *   EX-15  3 retransmissions sans ACK (hors INIT) : état ERREUR          -> retransmettre(), gerer_retransmission()
 *   EX-16  Header incomplet après 50 ms : abandon, recherche start_byte  -> lire_port()
 *   PC IN/OUT
 *   EX-17  Le master reçoit des messages en clair en entrée              -> sur_msg(), branche PORT_PC
 *   EX-18  Le dernier élément émet le message en clair                   -> envoyer_fifo()
 *   BOUTON USER
 *   EX-19  3 appuis sur USER : réinitialiser et se resynchroniser        -> gerer_bouton(), reinitialiser_et_synchroniser()
 *   LED
 *   EX-20  0,5 Hz en NON_INITIALISE                                      -> gerer_led()
 *   EX-21  2 Hz en EN_SYNCHRO                                            -> gerer_led()
 *   EX-22  5 Hz en ERREUR                                                -> gerer_led()
 *   EX-23  Fixe en SYNCHRONISE                                           -> gerer_led()
 *
 */

#include "main.h"
#include "carte.h"
#include <string.h>
#include <stddef.h>

#define HEADER_SIZE          sizeof(MessageHeader_t)  // 8 octets
#define CRC_SIZE             2
#define HEADER_TIMEOUT_MS    50     // [EX-16] header incomplet -> on abandonne la trame
#define FRAME_TIMEOUT_MS     100    // [COMPLÉMENT] payload/CRC incomplet -> on abandonne aussi (sinon blocage)
#define ACK_TIMEOUT_MS       200    // [EX-15] délai avant retransmission
#define MAX_RETRANSMISSIONS  3      // [EX-15]
#define RX_RING_SIZE         128
#define FIFO_SIZE            4      // messages en attente d'envoi en aval
#define BTN_DEBOUNCE_MS      30     // [EX-19]
#define BTN_FENETRE_MS       1500   // [EX-19] les 3 appuis doivent tenir dans cette fenêtre

// Code d'erreur mis dans le payload d'un CMD_ERROR
typedef enum {
  ERR_CRC    = 0x01,   // [EX-12]
  ERR_LENGTH = 0x02,   // [EX-13]
  ERR_CMD    = 0x03,   // [EX-14]
  ERR_PLEIN  = 0x04,   // [COMPLÉMENT] file d'attente pleine
  ERR_ETAT   = 0x05,   // [COMPLÉMENT] carte pas synchronisée (pas de clé pour traiter le message)
} CodeErreur_t;

typedef enum { PORT_PC, PORT_AMONT, PORT_AVAL, NB_PORTS } Port_t;

extern UART_HandleTypeDef huart2;   // PC
extern UART_HandleTypeDef huart4;   // aval
extern UART_HandleTypeDef huart5;   // amont

static UART_HandleTypeDef *const uart_du_port[NB_PORTS] = { &huart2, &huart5, &huart4 };

// Réception : un buffer circulaire par port, rempli sous interruption
typedef struct {
  uint8_t buf[RX_RING_SIZE];
  volatile uint16_t tete;    // écrit par l'interruption
  volatile uint16_t queue;   // lu par la boucle principale
  uint8_t octet;             // octet reçu par HAL_UART_Receive_IT
} RxRing_t;

// Découpage des trames, un par port
typedef enum { RX_ATTENTE_START, RX_HEADER, RX_CORPS } RxEtape_t;

typedef struct {
  RxEtape_t etape;
  uint8_t   trame[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
  uint8_t   idx;
  uint8_t   attendu;         // nombre total d'octets de la trame en cours
  uint32_t  t_debut;         // instant où le start_byte a été reçu
} Parser_t;

// Trame envoyée en aval qui attend son ACK
typedef struct {
  uint8_t  actif;
  uint8_t  cmd;
  uint8_t  payload[MAX_PAYLOAD_SIZE];
  uint8_t  len;
  uint8_t  essais;           // nombre de retransmissions déjà faites
  uint32_t t_envoi;
} Attente_t;

typedef struct {
  uint8_t data[MAX_PAYLOAD_SIZE];
  uint8_t len;
} Message_t;

Carte carte;

static RxRing_t  rx[NB_PORTS];
static Parser_t  parser[NB_PORTS];
static Attente_t attente;
static Message_t fifo[FIFO_SIZE];
static uint8_t   fifo_debut, fifo_nb;
static uint16_t  cle;
static uint8_t   dernier;              // 1 si la carte est la dernière de la chaîne
static Port_t    port_sortie_pc;       // où le dernier envoie les messages en clair
static volatile uint32_t graine;       // entropie pour la clé (instants de réception)

/* ---------------------------------------------------------------- outils */

// [EX-01] [EX-09] CRC-16/CCITT (poly 0x1021, init 0xFFFF), utilisé à l'émission et à la réception
static uint16_t crc16(const uint8_t *d, uint16_t n)
{
  uint16_t crc = 0xFFFF;
  while (n--)
  {
    crc ^= (uint16_t)(*d++) << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// [EX-11] XOR avec la clé : sert à chiffrer (master) et à déchiffrer (dernier slave)
static void xor_cle(uint8_t *data, uint8_t len)
{
  for (uint8_t i = 0; i < len; i++)
    data[i] ^= (i & 1) ? (uint8_t)(cle >> 8) : (uint8_t)(cle & 0xFF);
}

static uint32_t xorshift32(uint32_t x)
{
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

// [EX-05] Clé 16 bits aléatoire : compteur SysTick + tick + UID + instants des octets reçus
static uint16_t generer_cle(void)
{
  uint32_t x = graine ^ SysTick->VAL ^ (HAL_GetTick() << 16)
             ^ HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
  x = xorshift32(x ? x : 0x1234567u);
  graine = x;
  uint16_t k = (uint16_t)((x >> 16) ^ x);
  return k ? k : 0xA5C3;
}

/* ---------------------------------------------------------------- émission */

// [EX-01] Toute trame émise passe par ici : header (8 octets) + payload (len octets) + CRC (2 octets)
static void envoyer_trame(Port_t p, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
  uint8_t brut[HEADER_SIZE + MAX_PAYLOAD_SIZE + CRC_SIZE];
  MessageHeader_t h = {
    .start_byte   = ICD_START_BYTE,
    .command_type = cmd,
    .length       = len,
    .reserve      = 0,
    .id_src       = carte.id,
  };

  memcpy(brut, &h, HEADER_SIZE);                   // header
  if (len)
    memcpy(brut + HEADER_SIZE, payload, len);      // payload
  uint16_t crc = crc16(brut, HEADER_SIZE + len);   // CRC, poids faible d'abord
  brut[HEADER_SIZE + len]     = (uint8_t)(crc & 0xFF);
  brut[HEADER_SIZE + len + 1] = (uint8_t)(crc >> 8);

  HAL_UART_Transmit(uart_du_port[p], brut, HEADER_SIZE + len + CRC_SIZE, 100);
}

static void envoyer_erreur(Port_t p, CodeErreur_t code)
{
  uint8_t c = code;
  envoyer_trame(p, CMD_ERROR, &c, 1);
}

static void envoyer_ack(Port_t p)
{
  envoyer_trame(p, CMD_ACK, NULL, 0);
}

// [EX-15] Envoi en aval avec attente d'ACK (retransmis par gerer_retransmission)
static void envoyer_avec_ack(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
  attente.cmd = cmd;
  attente.len = len;
  memcpy(attente.payload, payload, len);
  attente.essais = 0;
  attente.actif = 1;
  attente.t_envoi = HAL_GetTick();
  envoyer_trame(PORT_AVAL, cmd, payload, len);
}

/* ---------------------------------------------------------------- file des messages */

// [COMPLÉMENT] file d'attente : un message reçu pendant qu'on attend un ACK en aval
// (ou pendant la synchro) n'est pas perdu

static uint8_t fifo_ajouter(const uint8_t *data, uint8_t len)
{
  if (fifo_nb == FIFO_SIZE)
    return 0;
  Message_t *m = &fifo[(fifo_debut + fifo_nb) % FIFO_SIZE];
  memcpy(m->data, data, len);
  m->len = len;
  fifo_nb++;
  return 1;
}

static void fifo_vider(void)
{
  fifo_debut = 0;
  fifo_nb = 0;
}

/* ---------------------------------------------------------------- états */

// [EX-15] état ERREUR (la LED passe à 5 Hz, EX-22)
static void passer_en_erreur(void)
{
  carte.etat = ERREUR;
  attente.actif = 0;
  fifo_vider();
}

// [EX-10] le nœud se sait dernier de la chaîne : il émettra les messages en clair (EX-11, EX-18)
static void devenir_dernier(Port_t sortie_pc)
{
  dernier = 1;
  port_sortie_pc = sortie_pc;
  carte.etat = SYNCHRONISE;
}

// [EX-06] Garde la clé et envoie l'INIT (CMD_INIT = 0x00) au nœud suivant, clé 16 bits dans le payload.
// Appelée par le master (nouvelle clé) et par chaque slave (EX-08 : propagation), donc
// l'INIT atteint chaque élément de la chaîne, quel que soit le nombre de cartes.
static void demarrer_synchro(uint16_t nouvelle_cle)
{
  uint8_t k[2] = { (uint8_t)(nouvelle_cle & 0xFF), (uint8_t)(nouvelle_cle >> 8) };  // clé, poids faible d'abord

  cle = nouvelle_cle;
  dernier = 0;
  fifo_vider();
  carte.etat = EN_SYNCHRO;                          // LED 2 Hz (EX-21)
  envoyer_avec_ack(CMD_INIT, k, sizeof(k));
}

// [EX-19] 3 appuis sur USER : le master relance la synchro de toute la chaîne,
// un slave se remet à zéro et demande au master (via RESET vers l'amont) de resynchroniser
static void reinitialiser_et_synchroniser(void)
{
  attente.actif = 0;
  fifo_vider();
  if (carte.role == MASTER)
  {
    demarrer_synchro(generer_cle());
  }
  else
  {
    carte.etat = NON_INITIALISE;
    dernier = 0;
    envoyer_trame(PORT_AMONT, CMD_RESET, NULL, 0);
  }
}

/* ---------------------------------------------------------------- réception des commandes */

static void sur_init(Port_t p, const ProtocolMessage_t *m)
{
  // l'INIT vient toujours du nœud précédent (master ou slave)
  if (p != PORT_AMONT)
    return;

  // [EX-06] l'INIT doit contenir la clé de 16 bits
  if (m->header.length < 2)
  {
    envoyer_erreur(p, ERR_LENGTH);
    return;
  }

  carte.role = SLAVE;                                            // [EX-03] INIT reçu d'une carte : slave
  envoyer_ack(PORT_AMONT);                                       // [EX-07] ACK à l'expéditeur
  demarrer_synchro((uint16_t)(m->payload[0] | (m->payload[1] << 8)));  // [EX-08] propagation en aval
}

// ACK reçu du nœud suivant, en réponse à l'INIT (ou au MSG) qu'on lui a envoyé
static void sur_ack(Port_t p, const ProtocolMessage_t *m)
{
  if (p != PORT_AVAL || !attente.actif)
    return;

  attente.actif = 0;                // [EX-15] ACK reçu : plus de retransmission
  if (attente.cmd == CMD_INIT)
  {
    // [EX-10] id_src NULL (0) : c'est le PC qui a répondu en aval, on est le dernier
    if (m->header.id_src == DEFAULT_ID)
    {
      devenir_dernier(PORT_AVAL);
    }
    else
    {
      dernier = 0;                  // une carte a répondu : on n'est pas le dernier
      carte.etat = SYNCHRONISE;     // LED fixe (EX-23)
    }
  }
}

// [EX-15] Retransmission d'une trame sans ACK ; au-delà de 3, état ERREUR
static void retransmettre(void)
{
  if (attente.essais >= MAX_RETRANSMISSIONS)
  {
    attente.actif = 0;
    if (attente.cmd == CMD_INIT)
      // [EX-15] hors phase INIT seulement
      // [EX-10] [COMPLÉMENT] personne ne répond en aval : on est le dernier, le PC est sur l'USART2
      devenir_dernier(PORT_PC);
    else
      passer_en_erreur();           // [EX-15] 3 retransmissions sans ACK -> ERREUR
    return;
  }
  attente.essais++;
  attente.t_envoi = HAL_GetTick();
  envoyer_trame(PORT_AVAL, attente.cmd, attente.payload, attente.len);
}

static void sur_erreur(Port_t p)
{
  // [EX-15] le nœud suivant a rejeté notre trame (EX-12/13/14) : on la renvoie tout de suite,
  // ça compte comme une retransmission
  if (p == PORT_AVAL && attente.actif)
    retransmettre();
}

static void sur_msg(Port_t p, const ProtocolMessage_t *m)
{
  uint8_t data[MAX_PAYLOAD_SIZE];
  uint8_t len = m->header.length;
  memcpy(data, m->payload, len);

  if (p == PORT_PC)
  {
    // [EX-04] [EX-17] message en clair venant du PC : id_src doit valoir 0x0.
    // Un slave déjà dans la chaîne l'ignore, pour qu'il n'y ait qu'un seul master.
    if (m->header.id_src != DEFAULT_ID || (carte.role == SLAVE && carte.etat != NON_INITIALISE))
      return;

    carte.role = MASTER;            // [EX-03] carte reliée au PC : master
    if (carte.etat == ERREUR)
    {
      envoyer_erreur(PORT_PC, ERR_ETAT);   // [COMPLÉMENT] en ERREUR, il faut d'abord resynchroniser (EX-19)
      return;
    }
    if (carte.etat == NON_INITIALISE)
      demarrer_synchro(generer_cle());     // [EX-05] [EX-06] première fois : clé + INIT de la chaîne

    xor_cle(data, len);             // [EX-11] le master chiffre le message clair avec la clé
    if (fifo_ajouter(data, len))
      envoyer_ack(PORT_PC);
    else
      envoyer_erreur(PORT_PC, ERR_PLEIN);
  }
  else if (p == PORT_AMONT)
  {
    // message chiffré venant du nœud précédent : relayé en aval, ou déchiffré si on est le dernier
    // EN_SYNCHRO : on a déjà la clé, le message attend dans la file la fin de la synchro en aval
    if (carte.etat != SYNCHRONISE && carte.etat != EN_SYNCHRO)
      envoyer_erreur(PORT_AMONT, ERR_ETAT);
    else if (fifo_ajouter(data, len))
      envoyer_ack(PORT_AMONT);
    else
      envoyer_erreur(PORT_AMONT, ERR_PLEIN);
  }
}

// [EX-19] CMD_RESET : c'est par lui qu'un slave dont on a appuyé 3 fois sur USER
// demande au master de resynchroniser la chaîne
static void sur_reset(Port_t p)
{
  // la demande remonte toujours du nœud suivant vers le master
  if (p != PORT_AVAL)
    return;

  envoyer_ack(PORT_AVAL);
  if (carte.role == MASTER)
    demarrer_synchro(generer_cle());                // le master relance l'INIT de toute la chaîne
  else
    envoyer_trame(PORT_AMONT, CMD_RESET, NULL, 0);  // un slave remonte la demande vers le master
}

static void traiter_trame(Port_t p, const uint8_t *brut, uint8_t taille)
{
  ProtocolMessage_t m;
  memcpy(&m.header, brut, HEADER_SIZE);
  memcpy(m.payload, brut + HEADER_SIZE, m.header.length);
  m.crc = (uint16_t)(brut[taille - 2] | (brut[taille - 1] << 8));

  // [EX-09] vérification du CRC à chaque trame reçue
  if (m.crc != crc16(brut, taille - CRC_SIZE))
  {
    // [EX-12] CRC invalide : trame rejetée + CMD_ERROR à l'émetteur (même port que la réception)
    // [COMPLÉMENT] pas de réponse à une trame d'erreur abîmée, pour éviter un ping-pong d'erreurs
    if (m.header.command_type != CMD_ERROR)
      envoyer_erreur(p, ERR_CRC);
    return;
  }

  switch (m.header.command_type)
  {
    case CMD_INIT:  sur_init(p, &m);  break;
    case CMD_RESET: sur_reset(p);     break;
    case CMD_ACK:   sur_ack(p, &m);   break;
    case CMD_MSG:   sur_msg(p, &m);   break;
    case CMD_ERROR: sur_erreur(p);    break;
    default:        envoyer_erreur(p, ERR_CMD); break;   // [EX-14] commande inconnue : rejet + CMD_ERROR
  }
}

/* ---------------------------------------------------------------- découpage des trames */

static uint8_t ring_lire(Port_t p, uint8_t *c)
{
  RxRing_t *r = &rx[p];
  if (r->queue == r->tete)
    return 0;
  *c = r->buf[r->queue];
  r->queue = (r->queue + 1) % RX_RING_SIZE;
  return 1;
}

static void lire_port(Port_t p)
{
  Parser_t *ps = &parser[p];
  uint32_t ecoule = HAL_GetTick() - ps->t_debut;
  uint8_t c;

  // [EX-16] header incomplet après 50 ms : on abandonne et on recherche le start_byte
  // [COMPLÉMENT] idem si le payload/CRC n'arrive pas en 100 ms
  if ((ps->etape == RX_HEADER && ecoule > HEADER_TIMEOUT_MS) ||
      (ps->etape == RX_CORPS  && ecoule > FRAME_TIMEOUT_MS))
    ps->etape = RX_ATTENTE_START;

  while (ring_lire(p, &c))
  {
    switch (ps->etape)
    {
      case RX_ATTENTE_START:
        // [EX-02] tout ce qui n'est pas le start_byte est ignoré
        if (c == ICD_START_BYTE)
        {
          ps->trame[0] = c;
          ps->idx = 1;
          ps->t_debut = HAL_GetTick();
          ps->etape = RX_HEADER;
        }
        break;

      case RX_HEADER:
        ps->trame[ps->idx++] = c;
        if (ps->idx == HEADER_SIZE)
        {
          uint8_t len = ps->trame[offsetof(MessageHeader_t, length)];
          if (len > MAX_PAYLOAD_SIZE)
          {
            // [EX-13] length > 16 : trame rejetée + CMD_ERROR
            ps->etape = RX_ATTENTE_START;
            envoyer_erreur(p, ERR_LENGTH);
          }
          else
          {
            ps->attendu = HEADER_SIZE + len + CRC_SIZE;
            ps->etape = RX_CORPS;
          }
        }
        break;

      case RX_CORPS:
        ps->trame[ps->idx++] = c;
        if (ps->idx == ps->attendu)
        {
          ps->etape = RX_ATTENTE_START;
          traiter_trame(p, ps->trame, ps->idx);
        }
        break;
    }
  }
}

/* ---------------------------------------------------------------- tâches de fond */

// [EX-15] pas d'ACK au bout de ACK_TIMEOUT_MS : retransmission
static void gerer_retransmission(void)
{
  if (attente.actif && HAL_GetTick() - attente.t_envoi >= ACK_TIMEOUT_MS)
    retransmettre();
}

// Envoie le prochain message de la file : relais en aval, ou en clair au PC si on est le dernier
static void envoyer_fifo(void)
{
  if (carte.etat != SYNCHRONISE || attente.actif || fifo_nb == 0)
    return;

  Message_t *m = &fifo[fifo_debut];
  fifo_debut = (fifo_debut + 1) % FIFO_SIZE;
  fifo_nb--;

  if (dernier)
  {
    xor_cle(m->data, m->len);       // [EX-11] le dernier déchiffre avec la clé...
    envoyer_trame(port_sortie_pc, CMD_MSG, m->data, m->len);   // [EX-11] [EX-18] ...et émet en clair au PC
  }
  else
  {
    envoyer_avec_ack(CMD_MSG, m->data, m->len);   // relais en aval, toujours chiffré
  }
}

// [EX-19] détection de 3 appuis sur le bouton USER (B1)
static void gerer_bouton(void)
{
  static uint8_t  stable = GPIO_PIN_SET, brut_prec = GPIO_PIN_SET;
  static uint32_t t_changement, t_premier_appui;
  static uint8_t  nb_appuis;
  uint32_t now = HAL_GetTick();
  uint8_t brut = HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin);

  // anti-rebond : le niveau doit rester stable BTN_DEBOUNCE_MS avant d'être pris en compte
  if (brut != brut_prec)
  {
    brut_prec = brut;
    t_changement = now;
  }
  else if (brut != stable && now - t_changement >= BTN_DEBOUNCE_MS)
  {
    stable = brut;
    if (stable == GPIO_PIN_RESET)   // B1 appuyé (actif à l'état bas)
    {
      if (nb_appuis == 0 || now - t_premier_appui > BTN_FENETRE_MS)
      {
        nb_appuis = 0;
        t_premier_appui = now;
      }
      if (++nb_appuis == 3)
      {
        nb_appuis = 0;
        reinitialiser_et_synchroniser();
      }
    }
  }
}

// LED : la demi-période (temps entre deux bascules) donne la fréquence
static void gerer_led(void)
{
  static uint32_t t_bascule;
  uint32_t demi_periode;

  switch (carte.etat)
  {
    case NON_INITIALISE: demi_periode = 1000; break;   // [EX-20] 0,5 Hz : période 2 s
    case EN_SYNCHRO:     demi_periode = 250;  break;   // [EX-21] 2 Hz   : période 500 ms
    case ERREUR:         demi_periode = 100;  break;   // [EX-22] 5 Hz   : période 200 ms
    default:
      carte.led_status = ON;                           // [EX-23] SYNCHRONISE : allumée fixe
      HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_SET);
      return;
  }

  carte.led_status = BLINK;
  if (HAL_GetTick() - t_bascule >= demi_periode)
  {
    t_bascule = HAL_GetTick();
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
  }
}

/* ---------------------------------------------------------------- API */

void Carte_Init(void)
{
  uint32_t id = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();

  carte.role = SLAVE;                          // [EX-03] devient MASTER sur message du PC
  carte.id = (id == DEFAULT_ID) ? 1 : id;      // [EX-04] [EX-10] l'id 0 est réservé au PC
  carte.etat = NON_INITIALISE;                 // LED 0,5 Hz (EX-20)
  carte.led_status = BLINK;
  port_sortie_pc = PORT_PC;

  // on demande à être prévenu (interruption) à chaque octet reçu, sur les 3 liaisons
  for (Port_t p = 0; p < NB_PORTS; p++)
    HAL_UART_Receive_IT(uart_du_port[p], &rx[p].octet, 1);
}

void Carte_Process(void)
{
  for (Port_t p = 0; p < NB_PORTS; p++)
    lire_port(p);
  gerer_retransmission();
  envoyer_fifo();
  gerer_bouton();
  gerer_led();
}

/* ---------------------------------------------------------------- interruptions UART */

// Appelée toute seule à chaque octet reçu : on le range et on relance l'écoute
// (le tri start_byte / header / payload / CRC est fait dans lire_port, EX-02)
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  for (Port_t p = 0; p < NB_PORTS; p++)
  {
    if (huart != uart_du_port[p])
      continue;

    RxRing_t *r = &rx[p];
    uint16_t suivant = (r->tete + 1) % RX_RING_SIZE;
    if (suivant != r->queue)        // buffer plein : octet perdu
    {
      r->buf[r->tete] = r->octet;
      r->tete = suivant;
    }
    graine = graine * 31u + SysTick->VAL;
    HAL_UART_Receive_IT(huart, &r->octet, 1);
  }
}

// Après une erreur (overrun, bruit...) la HAL coupe la réception : on la relance
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  for (Port_t p = 0; p < NB_PORTS; p++)
    if (huart == uart_du_port[p])
      HAL_UART_Receive_IT(huart, &rx[p].octet, 1);
}
