# Istruzioni per far girare la web app su un altro PC

Queste istruzioni servono per:
- primo avvio su un nuovo PC
- avvio giornaliero (ogni volta che vuoi usare l'app)
- risolvere il problema "sito web irraggiungibile"

## 1) Primo avvio su un nuovo PC

1. Copia tutto il progetto sul nuovo PC (cartella Progetto-APC).
2. Installa Python 3.11+ (consigliato 3.11 o 3.12).
3. Apri PowerShell e vai nella cartella del progetto:

```
cd c:\Users\NOME_UTENTE\Desktop\Progetto-APC
```

4. Crea il file .env a partire da .env.example:

```
cd pc_web_app
Copy-Item .env.example .env
```

5. Apri .env e compila questi valori:
- APP_ADMIN_USER
- APP_ADMIN_PASS
- ESP32_BASE_URL (esempio: http://192.168.1.50)
- ESP32_API_KEY (deve essere uguale a API_KEY nello sketch ESP32)

6. Installa le dipendenze:

```
cd c:\Users\NOME_UTENTE\Desktop\Progetto-APC
pip install -r pc_web_app\requirements.txt
```

7. Avvia la web app:

```
python pc_web_app\app.py
```

8. Apri il browser sullo stesso PC e vai a:
- http://localhost:5000

Se vuoi aprirla da un altro PC nella stessa rete, usa:
- http://IP_DEL_PC:5000

## 2) Avvio giornaliero (ogni volta)

1. Apri PowerShell.
2. Vai nella cartella del progetto:

```
cd c:\Users\NOME_UTENTE\Desktop\Progetto-APC
```

3. Avvia la web app:

```
python pc_web_app\app.py
```

4. Apri il browser:
- http://localhost:5000

## 3) Sito web irraggiungibile: checklist

1. La web app e davvero in esecuzione?
   - In PowerShell devi vedere:
     "Running on http://127.0.0.1:5000"
2. Stai usando l'indirizzo giusto?
   - Sullo stesso PC: http://localhost:5000
   - Da un altro PC: http://IP_DEL_PC:5000
3. Il firewall di Windows blocca la porta 5000?
   - Consenti Python nelle regole firewall (inbound).
4. Sei sulla stessa rete dell'altro PC?
   - Controlla che entrambi siano nella stessa rete Wi-Fi/LAN.
5. La porta 5000 e gia usata da un altro programma?
   - Chiudi il processo o cambia porta in app.py.
6. Il file .env e corretto?
   - Controlla ESP32_BASE_URL e ESP32_API_KEY.
7. L'ESP32 e raggiungibile?
   - Prova dal browser del PC: http://IP_ESP32/api/health
   - Se non risponde, verifica Wi-Fi e IP dell'ESP32.
