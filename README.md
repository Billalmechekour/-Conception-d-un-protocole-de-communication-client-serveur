# SAE - Conception d'un protocole de communication client-serveur

Projet SAE de conception et d'implementation d'un protocole de communication de type TFTP entre un serveur et plusieurs clients.

Le projet met l'accent sur:
- la fiabilite des echanges UDP (ACK, timeouts, retransmissions),
- la gestion des acces concurrents sur les fichiers,
- la robustesse face aux erreurs de communication.

## Objectifs

- Implementer les operations `GET` (RRQ) et `PUT` (WRQ).
- Gerer plusieurs clients en parallele.
- Ajouter la negociation d'options (`bigfile`, `windowsize`) sur la version threaded.
- Garantir la coherence des acces aux fichiers avec des verrous.

## Fonctionnalites

- Client TFTP:
- `get` et `put`
- mode `octet`
- options: `windowsize` (1..64) et `bigfile` (0|1)

- Serveur TFTP `threaded`:
- multi-clients via `pthread`
- support des options RFC 2347/7440 (`OACK`, `windowsize`, `bigfile`)
- gestion des erreurs TFTP

- Serveur TFTP `select`:
- multi-clients via boucle d'evenements `select()`
- gestion des transferts concurrents

- Verrouillage concurrent:
- verrous lecture/ecriture par fichier (`pthread_rwlock_t`)
- lectures concurrentes autorisees
- ecriture exclusive pour eviter les conflits et races

## Architecture

- `tftp_client.c`: logique client (GET/PUT)
- `tftp_server_threaded.c`: serveur multi-threads
- `tftp_server_select.c`: serveur event-loop `select()`
- `common.c` / `common.h`: primitives du protocole TFTP (paquets, options, utilitaires)
- `file_lock.c` / `file_lock.h`: table de verrous concurrents par nom de fichier

## Prerequis

- GCC
- Make
- Systeme POSIX (Linux/macOS)

## Compilation

```bash
make
```

Binaries generes:
- `bin/tftp_server_threaded`
- `bin/tftp_server_select`
- `bin/tftp_client`

## Execution

1. Creer un dossier de travail serveur:

```bash
mkdir -p /tmp/tftp
```

2. Lancer un serveur (exemple threaded):

```bash
./bin/tftp_server_threaded 6969 /tmp/tftp 1000
```

Alternative (version select):

```bash
./bin/tftp_server_select 6969 /tmp/tftp 1000
```

3. Telecharger un fichier (GET):

```bash
./bin/tftp_client get 127.0.0.1 fichier_distant.txt fichier_local.txt 6969 1000 4 1
```

4. Envoyer un fichier (PUT):

```bash
./bin/tftp_client put 127.0.0.1 fichier_local.txt fichier_distant.txt 6969 1000 4 1
```

Format general client:

```bash
./bin/tftp_client get <server_ip> <remote_file> <local_file> [port] [timeout_ms] [windowsize] [bigfile]
./bin/tftp_client put <server_ip> <local_file> <remote_file> [port] [timeout_ms] [windowsize] [bigfile]
```

Valeurs par defaut:
- `port=69`
- `timeout_ms=1000`
- `windowsize=1`
- `bigfile=0`

## Competences mobilisees

- Programmation reseau (sockets UDP)
- Conception de protocole applicatif
- Gestion de concurrence (threads, RW-lock)
- Fiabilite et tolerance aux erreurs
- Tests, debogage et validation client-serveur

## Auteur

Bilal Mechekour
