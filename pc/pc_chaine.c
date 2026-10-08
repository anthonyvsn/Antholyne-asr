/*
 * pc_chaine.c : programme PC pour envoyer / recevoir les trames de la chaîne de cartes
 *
 * Compilation (Mac ou Linux) :
 *   cc -Wall -o pc_chaine pc_chaine.c
 *
 * Utilisation :
 *   PC émetteur, branché sur la carte MASTER :
 *     ./pc_chaine envoyer /dev/tty.usbmodemXXXX "Bonjour"   -> envoie un message
 *     ./pc_chaine envoyer /dev/tty.usbmodemXXXX             -> tape les messages au clavier
 *
 *   PC récepteur, branché sur la DERNIÈRE carte de la chaîne :
 *     ./pc_chaine recevoir /dev/tty.usbmodemYYYY            -> affiche les messages en clair
 *
 * Pour trouver le port : ls /dev/tty.usbmodem*  (Mac)   ou   ls /dev/ttyACM*  (Linux)
 *
 * Trame : header (8 octets) + payload (length octets, 16 max) + CRC-16/CCITT (poids faible d'abord)
 * Un message de plus de 16 caractères est découpé en plusieurs trames.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <time.h>
#include "../Core/Src/protocol_icd.h"   // même format de trame que les cartes

#define HEADER_SIZE      sizeof(MessageHeader_t)
#define ACK_TIMEOUT_MS   3000   // le 1er message déclenche la synchro de la chaîne

// Configure le port série en 115200 bauds, 8 bits, pas de parité, 1 stop (comme l'USART2 des cartes)
static int configure_port(int fd)
{
  struct termios tty;

  if (tcgetattr(fd, &tty) != 0)
  {
    perror("tcgetattr");
    return -1;
  }

  cfmakeraw(&tty);                 // mode brut : les octets passent tels quels
  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);
  tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
  tty.c_cflag |= CS8 | CLOCAL | CREAD;
  tty.c_cc[VMIN]  = 0;             // read() rend la main...
  tty.c_cc[VTIME] = 1;             // ...après 100 ms sans octet

  if (tcsetattr(fd, TCSANOW, &tty) != 0)
  {
    perror("tcsetattr");
    return -1;
  }
  return 0;
}

static uint32_t maintenant_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// Même CRC que les cartes : CRC-16/CCITT, poly 0x1021, init 0xFFFF
static uint16_t crc16(const uint8_t *d, uint16_t n)
{
  uint16_t crc = 0xFFFF;
  while (n--)
  {
    crc ^= (uint16_t)(*d++) << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Envoie une trame avec id_src = 0 (DEFAULT_ID) : c'est l'id du PC
static void envoyer_trame(int fd, uint8_t cmd, const uint8_t *payload, uint8_t len)
{
  uint8_t brut[HEADER_SIZE + MAX_PAYLOAD_SIZE + 2];
  MessageHeader_t h = { ICD_START_BYTE, cmd, len, 0, DEFAULT_ID };

  memcpy(brut, &h, HEADER_SIZE);
  memcpy(brut + HEADER_SIZE, payload, len);
  uint16_t crc = crc16(brut, HEADER_SIZE + len);
  brut[HEADER_SIZE + len]     = crc & 0xFF;
  brut[HEADER_SIZE + len + 1] = crc >> 8;
  write(fd, brut, HEADER_SIZE + len + 2);
}

// Lit un octet, en abandonnant à l'instant "limite" ; renvoie 0 si rien n'est arrivé
static int lire_octet(int fd, uint8_t *c, uint32_t limite)
{
  while ((int32_t)(limite - maintenant_ms()) > 0)
    if (read(fd, c, 1) == 1)
      return 1;
  return 0;
}

// Attend une trame complète. Renvoie 1 = trame valide, 0 = rien reçu, -1 = CRC faux
static int lire_trame(int fd, ProtocolMessage_t *m, uint32_t timeout_ms)
{
  uint8_t brut[HEADER_SIZE + MAX_PAYLOAD_SIZE + 2];
  uint32_t limite = maintenant_ms() + timeout_ms;

  for (;;)
  {
    // on ignore tout jusqu'au start_byte
    do
    {
      if (!lire_octet(fd, &brut[0], limite))
        return 0;
    } while (brut[0] != ICD_START_BYTE);

    // header : 50 ms maximum, comme sur les cartes
    uint32_t fin_header = maintenant_ms() + 50;
    size_t i;
    for (i = 1; i < HEADER_SIZE; i++)
      if (!lire_octet(fd, &brut[i], fin_header))
        break;
    if (i < HEADER_SIZE)
      continue;                     // header incomplet : on recherche le start_byte

    uint8_t len = brut[offsetof(MessageHeader_t, length)];
    if (len > MAX_PAYLOAD_SIZE)
      continue;

    uint32_t fin_corps = maintenant_ms() + 100;
    for (i = 0; i < (size_t)len + 2; i++)
      if (!lire_octet(fd, &brut[HEADER_SIZE + i], fin_corps))
        break;
    if (i < (size_t)len + 2)
      continue;

    memcpy(&m->header, brut, HEADER_SIZE);
    memcpy(m->payload, brut + HEADER_SIZE, len);
    m->crc = brut[HEADER_SIZE + len] | (brut[HEADER_SIZE + len + 1] << 8);
    return (m->crc == crc16(brut, HEADER_SIZE + len)) ? 1 : -1;
  }
}

static const char *nom_erreur(uint8_t code)
{
  switch (code)
  {
    case 1:  return "CRC invalide";
    case 2:  return "length > 16";
    case 3:  return "commande inconnue";
    case 4:  return "file d'attente pleine";
    case 5:  return "carte pas synchronisee (en ERREUR : 3 appuis sur USER)";
    default: return "inconnue";
  }
}

// Envoie un texte au master, découpé en trames de 16 octets, et attend l'ACK de chacune
static void envoyer_message(int fd, const char *texte)
{
  size_t total = strlen(texte);

  for (size_t debut = 0; debut < total; debut += MAX_PAYLOAD_SIZE)
  {
    uint8_t len = (total - debut > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : (uint8_t)(total - debut);
    ProtocolMessage_t rep;

    envoyer_trame(fd, CMD_MSG, (const uint8_t *)texte + debut, len);
    printf("-> envoye : \"%.*s\"\n", len, texte + debut);

    int r = lire_trame(fd, &rep, ACK_TIMEOUT_MS);
    if (r == 0)
      printf("   pas de reponse du master (port ? carte flashee ?)\n");
    else if (r < 0)
      printf("   reponse avec un CRC faux\n");
    else if (rep.header.command_type == CMD_ACK)
      printf("   ACK du master (id 0x%08X)\n", rep.header.id_src);
    else if (rep.header.command_type == CMD_ERROR)
      printf("   ERREUR du master : %s\n", nom_erreur(rep.payload[0]));
    else
      printf("   reponse inattendue (commande %d)\n", rep.header.command_type);
  }
}

static int mode_envoyer(int fd, const char *texte)
{
  char ligne[256];

  if (texte)
  {
    envoyer_message(fd, texte);
    return 0;
  }

  printf("Tape un message puis Entree (Ctrl+D pour quitter)\n");
  while (fgets(ligne, sizeof(ligne), stdin))
  {
    ligne[strcspn(ligne, "\r\n")] = '\0';
    if (ligne[0])
      envoyer_message(fd, ligne);
  }
  return 0;
}

static int mode_recevoir(int fd)
{
  ProtocolMessage_t m;

  printf("En attente des messages de la derniere carte (Ctrl+C pour quitter)\n");
  for (;;)
  {
    int r = lire_trame(fd, &m, 1000);
    if (r == 0)
      continue;
    if (r < 0)
    {
      printf("<- trame recue avec un CRC faux, ignoree\n");
      continue;
    }

    if (m.header.command_type == CMD_MSG)
      printf("<- message en clair de la carte 0x%08X : \"%.*s\"\n",
             m.header.id_src, m.header.length, (const char *)m.payload);
    else
      printf("<- trame commande %d de la carte 0x%08X\n", m.header.command_type, m.header.id_src);
  }
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IOLBF, 0); // chaque ligne s'affiche tout de suite
  if (argc < 3 || (strcmp(argv[1], "envoyer") != 0 && strcmp(argv[1], "recevoir") != 0))
  {
    fprintf(stderr, "Utilisation :\n"
                    "  %s envoyer  <port> [\"message\"]\n"
                    "  %s recevoir <port>\n", argv[0], argv[0]);
    return 1;
  }

  // O_NONBLOCK : sur Mac, ouvrir /dev/tty.* attend sinon un signal "porteuse" qui ne vient jamais
  int fd = open(argv[2], O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
  {
    perror(argv[2]);
    return 1;
  }
  if (configure_port(fd) != 0)      // CLOCAL : on ignore la porteuse à partir de maintenant
    return 1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);   // retour en mode bloquant (VTIME = 100 ms)
  tcflush(fd, TCIOFLUSH);           // on part d'un port vide

  int ret = (argv[1][0] == 'e') ? mode_envoyer(fd, argc > 3 ? argv[3] : NULL)
                                : mode_recevoir(fd);
  close(fd);
  return ret;
}
