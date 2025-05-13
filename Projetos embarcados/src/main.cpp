#include <Arduino.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <time.h>

/* Define wifi credentials */
#define WIFI_SSID "uaifai-tiradentes"
#define WIFI_PASSWORD "bemvindoaocesar"

/* Define database credentials */

/* Defines Firebase Data Object */
FirebaseData firebaseData;

/* Defines Firebase Auth */
FirebaseAuth firebaseAuth;

/* Defines Firebase Config */
FirebaseConfig firebaseConfig;

// Configuração NTP para obter a hora
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -3 * 3600;  // GMT-3 para horário brasileiro
const int   daylightOffset_sec = 0;

//----------------------------------------Include Library
//----------------------------------------
// Monitor Cardíaco Simplificado - Apenas com output Serial
//----------------------------------------

// Defines the PIN used.
#define PulseSensor_PIN 36  // VP/GPIO36 para o sensor cardíaco
#define LED_PIN         23  // LED para indicar batimentos
#define Button_PIN      32  // Botão para iniciar/parar a medição

unsigned long previousMillisGetHB = 0; // Armazena o último tempo para leitura do batimento
unsigned long previousMillisResultHB = 0; // Armazena o último tempo para cálculo do BPM
unsigned long previousMillisSerialOutput = 0; // Para controlar a saída serial do sinal

const long intervalGetHB = 35; // Intervalo para leitura dos batimentos = 35ms
const long intervalResultHB = 1000; // Intervalo para cálculo do BPM = 1000ms (1 segundo)
const long intervalSerialOutput = 100; // Intervalo para imprimir o sinal no Serial = 100ms

int timer_Get_BPM = 0;

int PulseSensorSignal; // Variável para acomodar o valor do sinal do sensor
int UpperThreshold = 520; // Determina qual sinal "contar como batimento" e qual ignorar
int LowerThreshold = 500;

int cntHB = 0; // Variável para contar o número de batimentos
boolean ThresholdStat = true; // Variável para gatilhos no cálculo de batimentos
int BPMval = 0; // Variável para armazenar o resultado do cálculo dos batimentos

// Variável booleana para iniciar e parar a obtenção de valores de BPM
bool get_BPM = false;

void setupNTP() {
  Serial.println("\nConfigurando servidor NTP...");
  
  // Configura o servidor NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Espera até obter uma hora válida
  struct tm timeinfo;
  int retry = 0;
  while(!getLocalTime(&timeinfo) && retry < 5) {
    Serial.println("Falha ao obter tempo do servidor NTP. Tentando novamente...");
    delay(1000);
    retry++;
  }
  
  if (retry < 5) {
    Serial.println("Servidor NTP configurado!");
    Serial.print("Hora atual: ");
    Serial.println(&timeinfo, "%H:%M:%S");
  } else {
    Serial.println("Não foi possível obter a hora do servidor NTP.");
  }
}

void setupFirebase() {
  Serial.println("\nConfigurando Firebase...");
  
  // Configure Firebase credentials
  firebaseConfig.database_url = "https://embarcados-2ca49-default-rtdb.firebaseio.com/";
  firebaseConfig.signer.tokens.legacy_token = "q1EWzv5skObBytM10H00TiQJCTlH8UaEtavKWkNm";

  Firebase.reconnectNetwork(true);

  firebaseData.setBSSLBufferSize(4096 /* Rx buffer size in bytes from 512 - 16384 */, 1024 /* Tx buffer size in bytes from 512 - 16384 */);
  
  // Initialize Firebase
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Serial.println("Firebase configurado!");
}

void connectWiFi() {
  Serial.println("\nConectando ao WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha ao conectar ao WiFi. Verificar credenciais.");
  }
}

String getCurrentTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) {
    Serial.println("Falha ao obter a hora atual");
    return "00:00:00";  // Retorna horário padrão em caso de falha
  }
  
  char timeString[9];  // HH:MM:SS + null terminator
  sprintf(timeString, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(timeString);
}

void sendToFirebase(int bpmValue) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi desconectado. Tentando reconectar...");
    connectWiFi();
    return;
  }
  
  // Obter a hora atual
  String currentTime = getCurrentTime();
  
  // Criar objeto JSON para enviar ao Firebase
  FirebaseJson json;
  json.set("bpm", bpmValue);
  json.set("timestamp", currentTime);
  
  // Gerar uma chave única baseada no timestamp para não sobrescrever dados anteriores
  String uniqueKey = String(millis());
  
  // Caminho para salvar os dados no Firebase
  String path = "/heartrate/" + uniqueKey;
  
  // Enviar dados para o Firebase
  if (Firebase.setJSON(firebaseData, path, json)) {
    Serial.println("Dados enviados para o Firebase com sucesso:");
    Serial.print("  {\"bpm\": ");
    Serial.print(bpmValue);
    Serial.print(", \"timestamp\": \"");
    Serial.print(currentTime);
    Serial.println("\"}");
  } else {
    Serial.println("Falha ao enviar dados para o Firebase:");
    Serial.println(firebaseData.errorReason());
  }
}

//________________________________________________________________________________
void GetHeartRate() {
  //----------------------------------------Processo de leitura do batimento cardíaco
  unsigned long currentMillisGetHB = millis();

  if (currentMillisGetHB - previousMillisGetHB >= intervalGetHB) {
    previousMillisGetHB = currentMillisGetHB;

    PulseSensorSignal = analogRead(PulseSensor_PIN); // Lê o valor do sensor

    if (PulseSensorSignal > UpperThreshold && ThresholdStat == true) {
      if (get_BPM == true) cntHB++;
      ThresholdStat = false;
      digitalWrite(LED_PIN, HIGH);
    }

    if (PulseSensorSignal < LowerThreshold) {
      ThresholdStat = true;
      digitalWrite(LED_PIN, LOW);
    }
  }
  //----------------------------------------

  //----------------------------------------Imprimir o valor do sinal no terminal
  unsigned long currentMillisSerialOutput = millis();
  if (currentMillisSerialOutput - previousMillisSerialOutput >= intervalSerialOutput && get_BPM) {
    previousMillisSerialOutput = currentMillisSerialOutput;
    //Serial.print("Sinal: ");
    //Serial.println(PulseSensorSignal);
  }
  //----------------------------------------

  //----------------------------------------Processo para obter o valor de BPM
  unsigned long currentMillisResultHB = millis();

  if (currentMillisResultHB - previousMillisResultHB >= intervalResultHB) {
    previousMillisResultHB = currentMillisResultHB;

    if (get_BPM == true) {
      timer_Get_BPM++;
      // "timer_Get_BPM > 10" significa contar os batimentos por 10 segundos
      if (timer_Get_BPM > 10) {
        timer_Get_BPM = 1;

        BPMval = cntHB * 6; // O batimento cardíaco é medido por 10 segundos. Para obter o valor BPM, batimentos totais em 10 segundos x 6
        Serial.println("------------------------");
        Serial.print("Batimentos detectados (10s): ");
        Serial.println(cntHB);
        Serial.print("BPM: ");
        Serial.println(BPMval);
        Serial.println("------------------------");
        sendToFirebase(BPMval); // Envia o valor de BPM para o Firebase
        cntHB = 0;
      }
    }
  }
  //----------------------------------------
}
//________________________________________________________________________________

//________________________________________________________________________________
void setup() {
  Serial.begin(115200);
  Serial.println();
  delay(1000);
  
  Serial.println("=================================");
  Serial.println("  ESP32 - MONITOR CARDÍACO");
  Serial.println("=================================");
  Serial.println("Inicializando...");
  
  analogReadResolution(10);

  pinMode(LED_PIN, OUTPUT); 
  pinMode(Button_PIN, INPUT_PULLUP);

  // Conectar ao WiFi
  connectWiFi();
  
  // Configurar NTP para obter a hora
  setupNTP();
  
  // Configurar Firebase
  setupFirebase();

  Serial.println("Sistema de monitoramento cardíaco iniciado!");
  Serial.println("Pressione o botão para iniciar/parar a medição.");
  Serial.println("=================================");
}
//________________________________________________________________________________

//________________________________________________________________________________
void loop() {
  // Verifica se o botão foi pressionado
  if (digitalRead(Button_PIN) == LOW) {
    delay(200); // Debounce para evitar múltiplos acionamentos
    
    // Inverte o estado da leitura
    get_BPM = !get_BPM;

    if (get_BPM) {
      // Reinicia as variáveis
      cntHB = 0;
      BPMval = 0;
      timer_Get_BPM = 0;
      
      Serial.println("\n>> INICIANDO MEDIÇÃO <<");
      Serial.println("Calculando BPM... aguarde 10 segundos para o primeiro resultado");
    } else {
      Serial.println("\n>> MEDIÇÃO FINALIZADA <<");
      if (BPMval > 0) {
        Serial.print("Último valor de BPM registrado: ");
        Serial.println(BPMval);
      }
    }
    
    // Aguarda a liberação do botão
    while (digitalRead(Button_PIN) == LOW) {
      delay(10);
    }
  }

  GetHeartRate(); // Chama a subrotina GetHeartRate()
}
//________________________________________________________________________________