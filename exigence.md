Le logiciel doit émettre chaque trame au bon format : header + payload + CRC.
Le logiciel doit ignorer tous les octets reçus tant qu'il n'a pas détecté le start_byte.
Le logiciel doit attribuer le rôle master à la carte reliée au PC, et le rôle slave aux autres cartes.

%% MASTER
Le logiciel doit attribuer un rôle (master/slave), le master reçoit un message clair avec l'id 0x0 qui vient du PC émetteur.

Le logiciel doit générer de manière aléatoire une clé de 16 bits.

Le logiciel doit envoyer un message d'init (0X00) à chaque élément de la chaîne avec dans le payload un message de 16 bits qui contient la clé.

Le logiciel doit sur un message d'INIT envoyer un message un message ACK à l'expéditeur.

%% SLAVES
Le logiciel doit permettre au slave de propager un message d'INIT au autre slave connecter.

Le logiciel vérifie le crc lorsqu’il reçoit un message pour vérifier l'intégrité des données.

Le logiciel permet de détecter si le nœud est le dernier de la chaîne, pour cela il envoie un ACK au nœud suivant s' il s'agit du pc le source id vaudra NULL. 

Le logiciel doit déchiffrer le message à l'aide de la clé si le slaves est le dernier de la chaîne et le transmet en clair au PC. 

%% ERREUR
Sur CRC invalide, le logiciel doit rejeter la trame et répondre CMD_ERROR à l'émetteur.
Sur length > 16, le logiciel doit rejeter la trame et répondre CMD_ERROR.
Sur commande inconnue, le logiciel doit rejeter la trame et répondre CMD_ERROR.
Après 3 retransmissions sans ACK (hors phase INIT), le logiciel doit passer à l'état ERREUR.
Si le header est incomplet après 50 ms, le logiciel doit abandonner la trame en cours et se remettre en recherche du start_byte.

%% PC IN/OUT
Le logiciel doit permettre au master de recevoir des messages en clair en entrée.

Le logiciel doit émettre un message en clair lorsqu’il s’agit du dernier élément de la chaîne. 

%% BOUTON USER
Le logiciel doit permettre de réinitialiser et de se synchroniser à la chaîne en appuyant 3 fois sur le boutons USER. 

%% LED
La LED doit clignoter à 0,5 Hz en état NON_INITIALISE.
La LED doit clignoter à 2 Hz en état EN_SYNCHRO.
La LED doit clignoter à 5 Hz en état ERREUR lors du traitement des messages.
La LED doit être allumée fixe en état SYNCHRONISE.

