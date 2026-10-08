#ifndef CARTE_H
#define CARTE_H

#include <stdint.h>
#include "protocol_icd.h"   // ICD_START_BYTE, MAX_PAYLOAD_SIZE, DEFAULT_ID, trames


typedef enum {
   MASTER = 0x00, // Carte maître
   SLAVE  = 0x01, // Carte esclave
} RoleType_t;

typedef enum {
   ON = 0x00, //LED ON
   OFF = 0x01, // LED OFF
   BLINK = 0x02, // LED BLINK
} LED_Status_t;

// État de la carte dans la chaîne (pilote le clignotement de la LED)
typedef enum {
   NON_INITIALISE = 0x00, // LED 0,5 Hz
   EN_SYNCHRO     = 0x01, // LED 2 Hz
   SYNCHRONISE    = 0x02, // LED fixe
   ERREUR         = 0x03, // LED 5 Hz
} EtatCarte_t;

typedef struct __attribute__((packed)) {
    RoleType_t role;        // Rôle de la carte (1 octet)
    uint32_t   id;          // Identifiant unique de la carte (4 octets)
    LED_Status_t led_status; // État de la LED (1 octet)
    EtatCarte_t  etat;       // État dans la chaîne
} Carte;

// À appeler une fois, après l'init des UART (huart2 = PC, huart5 = amont, huart4 = aval)
void Carte_Init(void);

// À appeler en boucle dans le while(1) du main
void Carte_Process(void);

#endif
