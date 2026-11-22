Check release !!!!!!


CAMERA THERMIQUE UNI-T UTi120Mobile - R36S (dArkOS)  -  version 2
=================================================================

INSTALLATION
  Copiez Thermique.sh et uti120.c dans le meme dossier "ports" de la carte SD.
  (Supprimez les anciens fichiers uti120_new.c et uti120M.sh : ils ne servent pas.)
  Branchez la camera directement sur le port OTG, puis lancez "Thermique".

PREMIER LANCEMENT
  L'assistant vous demande 12 boutons (haut, bas, gauche, droite, OK, retour, menu,
  capture, palette, zoom +, zoom -, geler). Boutons, croix et directions des sticks
  sont acceptes. Un bouton deja pris est refuse. "RETOUR" = passer.
  La calibration de la camera se fait en meme temps (10 a 20 secondes).

EN UTILISATION
  Croix / stick gauche : deplace le viseur (maintenir = plus vite)
  Stick droit          : haut/bas = zoom, gauche/droite = palette
  Menu                 : tous les reglages (image, mesure, temperature, camera...)
  Retour (2 fois)      : quitter.  Maintenir un bouton 5 s = arret de securite.

AUTRES FONCTIONS (a assigner dans MENU > BOUTONS si vous le souhaitez)
  Gel de l'image, recalibration, affichage, plage auto, viseur au centre,
  reference (delta), mesure point/zone, aide, galerie, niveau et contraste.

TEMPERATURES
  La camera ne donne pas de degres : ils sont estimes, donc approximatifs.
  Pour des valeurs plus fiables : MENU > TEMPERATURE > ETALON PT 1 (eau glacee = 0 C),
  puis ETALON PT 2 sur une cible chaude connue. Reglez aussi le MATERIAU (emissivite).

SI QUELQUE CHOSE NE VA PAS
  MENU > MANETTE > TEST MANETTE : montre les numeros des axes de vos sticks.
  Affichage bizarre : creez un fichier vide  uti120_out/rendu_logiciel
  Envoyez-moi : uti120_out/log_affichage.txt (ou log_precedent.txt), build.txt, settings.cfg
