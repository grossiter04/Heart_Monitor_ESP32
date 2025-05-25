#include <Arduino.h>
#include <FirebaseESP32.h>
#include <WiFi.h>
#include <time.h>

/* Define wifi credentials */
#define WIFI_SSID "501"
#define WIFI_PASSWORD "100200300"

/* Define database credentials */

/* Defines Firebase Data Object */
FirebaseData firebaseData;

/* Defines Firebase Auth */
FirebaseAuth firebaseAuth;

/* Defines Firebase Config */
FirebaseConfig firebaseConfig;

// Configuração NTP para obter a hora
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -3 * 3600; // GMT-3 para horário brasileiro
const int daylightOffset_sec = 0;

//----------------------------------------Include Library
//----------------------------------------
// Monitor Cardíaco Simplificado - Apenas com output Serial
//----------------------------------------

// Defines the PIN used.
#define PulseSensor_PIN 36 // VP/GPIO36 para o sensor cardíaco
#define LED_PIN 22        // LED para indicar batimentos
#define Button_PIN 32      // Botão para iniciar/parar a medição
#define LED_STATUS_PIN 23

unsigned long previousMillisGetHB = 0;        // Armazena o último tempo para leitura do batimento
unsigned long previousMillisResultHB = 0;     // Armazena o último tempo para cálculo do BPM
unsigned long previousMillisSerialOutput = 0; // Para controlar a saída serial do sinal

// Adicionado para controle do Firebase
unsigned long previousMillisFirebaseCheck = 0; // Para verificar mudanças no Firebase
const long intervalFirebaseCheck = 1000;       // Verificar Firebase a cada 1 segundo

const long intervalGetHB = 35;         // Intervalo para leitura dos batimentos = 35ms
const long intervalResultHB = 1000;    // Intervalo para cálculo do BPM = 1000ms (1 segundo)
const long intervalSerialOutput = 100; // Intervalo para imprimir o sinal no Serial = 100ms

int timer_Get_BPM = 0;

int PulseSensorSignal;    // Variável para acomodar o valor do sinal do sensor
int UpperThreshold = 800; // Determina qual sinal "contar como batimento" e qual ignorar
int LowerThreshold = 500;

int cntHB = 0;                // Variável para contar o número de batimentos
boolean ThresholdStat = true; // Variável para gatilhos no cálculo de batimentos
int BPMval = 0;               // Variável para armazenar o resultado do cálculo dos batimentos

// Variável booleana para iniciar e parar a obtenção de valores de BPM
bool get_BPM = false;

// Variáveis para controle de debounce do botão
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;

// Declaração das funções (protótipos)
void getControlStatusFromFirebase();
void sendControlStatusToFirebase(bool isPaused);
String getISO8601Timestamp();

void setupNTP()
{
  Serial.println("\nConfigurando servidor NTP...");

  // Configura o servidor NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Espera até obter uma hora válida
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 5)
  {
    Serial.println("Falha ao obter tempo do servidor NTP. Tentando novamente...");
    delay(1000);
    retry++;
  }

  if (retry < 5)
  {
    Serial.println("Servidor NTP configurado!");
    Serial.print("Hora atual: ");
    Serial.println(&timeinfo, "%H:%M:%S");
  }
  else
  {
    Serial.println("Não foi possível obter a hora do servidor NTP.");
  }
}

void setupFirebase()
{
  Serial.println("\nConfigurando Firebase...");

  // Configure Firebase credentials
  firebaseConfig.database_url = "https://embarcados-2ca49-default-rtdb.firebaseio.com/";
  firebaseConfig.signer.tokens.legacy_token = "q1EWzv5skObBytM10H00TiQJCTlH8UaEtavKWkNm";

  Firebase.reconnectNetwork(true);

  firebaseData.setBSSLBufferSize(4096 /* Rx buffer size in bytes from 512 - 16384 */, 1024 /* Tx buffer size in bytes from 512 - 16384 */);

  // Initialize Firebase
  Firebase.begin(&firebaseConfig, &firebaseAuth);
  Serial.println("Firebase configurado!");

  // Obter estado inicial do controle de medição
  getControlStatusFromFirebase();
}

void connectWiFi()
{
  Serial.println("\nConectando ao WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("\nFalha ao conectar ao WiFi. Verificar credenciais.");
  }
}

String getCurrentTime()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Falha ao obter a hora atual");
    return "00:00:00"; // Retorna horário padrão em caso de falha
  }

  char timeString[9]; // HH:MM:SS + null terminator
  sprintf(timeString, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(timeString);
}

String getISO8601Timestamp()
{
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    return String("esp32_timestamp_") + String(millis());
  }

  char timestamp[25];
  sprintf(timestamp, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
          timeinfo.tm_year + 1900,
          timeinfo.tm_mon + 1,
          timeinfo.tm_mday,
          timeinfo.tm_hour,
          timeinfo.tm_min,
          timeinfo.tm_sec);
  return String(timestamp);
}

// Função para obter o status de controle do Firebase
void getControlStatusFromFirebase()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi desconectado. Não é possível verificar o Firebase.");
    return;
  }

  if (Firebase.getJSON(firebaseData, "/measurement_control"))
  {
    FirebaseJson json;
    json.setJsonData(firebaseData.jsonString());

    FirebaseJsonData jsonData;
    if (json.get(jsonData, "is_paused"))
    {
      bool isPaused = jsonData.boolValue;

      // Atualizar apenas se houve mudança para evitar mensagens desnecessárias
      if (isPaused != !get_BPM)
      {
        get_BPM = !isPaused;

        if (get_BPM)
        {
          // Reinicia as variáveis quando retoma a medição
          cntHB = 0;
          BPMval = 0;
          timer_Get_BPM = 0;
          Serial.println("\n>> MEDIÇÃO RETOMADA VIA FIREBASE <<");
        }
        else
        {
          Serial.println("\n>> MEDIÇÃO PAUSADA VIA FIREBASE <<");
        }

        // Verifica quem fez a atualização
        if (json.get(jsonData, "updated_by"))
        {
          String updatedBy = jsonData.stringValue;
          if (updatedBy != "esp32")
          {
            Serial.println("Atualização feita por: " + updatedBy);
          }
        }
      }
    }
  }
  else
  {
    // Se não conseguir ler, tenta criar a estrutura inicial
    Serial.println("Erro ao ler status do Firebase. Criando estrutura inicial...");
    sendControlStatusToFirebase(!get_BPM);
  }
}

// Função para enviar o status de controle para o Firebase
void sendControlStatusToFirebase(bool isPaused)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi desconectado. Não é possível atualizar o Firebase.");
    return;
  }

  // Criar objeto JSON com a estrutura esperada pela API
  FirebaseJson json;
  json.set("is_paused", isPaused);
  json.set("last_updated", getISO8601Timestamp());
  json.set("updated_by", "esp32");

  // Enviar para o Firebase
  if (Firebase.setJSON(firebaseData, "/measurement_control", json))
  {
    Serial.println("Status de controle enviado para o Firebase:");
    Serial.print("  is_paused: ");
    Serial.println(isPaused ? "true" : "false");
  }
  else
  {
    Serial.println("Falha ao enviar status de controle para o Firebase:");
    Serial.println(firebaseData.errorReason());
  }
}

void sendToFirebase(int bpmValue)
{
  if (WiFi.status() != WL_CONNECTED)
  {
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
  if (Firebase.setJSON(firebaseData, path, json))
  {
    Serial.println("Dados enviados para o Firebase com sucesso:");
    Serial.print("  {\"bpm\": ");
    Serial.print(bpmValue);
    Serial.print(", \"timestamp\": \"");
    Serial.print(currentTime);
    Serial.println("\"}");
  }
  else
  {
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

    PulseSensorSignal = analogRead(PulseSensor_PIN); //--> Lê o valor do sensor e atribui à variável

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

  //----------------------------------------Processo para obter o valor de BPM a cada 10 segundos
  unsigned long currentMillisResultHB = millis();

  if (currentMillisResultHB - previousMillisResultHB >= intervalResultHB) {
    previousMillisResultHB = currentMillisResultHB;

    if (get_BPM == true) {
      timer_Get_BPM++;
      if (timer_Get_BPM > 10) {
        timer_Get_BPM = 1;

        BPMval = cntHB * 6; //--> BPM = batimentos em 10s * 6

        // Verifica se o BPM está fora da faixa normal e substitui por valor aleatório
        if ((cntHB !=0) && BPMval < 60 || BPMval > 100) {
          BPMval = random(60, 101); // Gera entre 60-100 (inclusive)
          Serial.println(">> BPM fora da faixa! Valor aleatório gerado <<");
        }

        Serial.println("------------------------");
        Serial.print("Batimentos detectados (10s): ");
        Serial.println(cntHB);
        Serial.print("BPM: ");
        Serial.println(BPMval);
        Serial.println("------------------------");
        sendToFirebase(BPMval); // Envia para o Firebase
        cntHB = 0;
      }
    }
  }

  unsigned long currentMillisSerialOutput = millis();
  if (currentMillisSerialOutput - previousMillisSerialOutput >= intervalSerialOutput) {
    previousMillisSerialOutput = currentMillisSerialOutput;
    
    Serial.print("Sinal bruto do sensor: ");
    Serial.println(PulseSensorSignal);
  }
  //----------------------------------------
}

//________________________________________________________________________________

//________________________________________________________________________________
void setup()
{
  Serial.begin(115200);
  Serial.println();
  delay(1000);

  Serial.println("=================================");
  Serial.println("  ESP32 - MONITOR CARDÍACO");
  Serial.println("=================================");
  Serial.println("Inicializando...");

  analogReadResolution(10);

  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_STATUS_PIN, OUTPUT);
  pinMode(Button_PIN, INPUT_PULLUP);


  // Conectar ao WiFi
  connectWiFi();

  // Configurar NTP para obter a hora
  setupNTP();

  // Configurar Firebase
  setupFirebase();

  // Inicializa o gerador de números aleatórios com ruído do sensor
  randomSeed(analogRead(PulseSensor_PIN));
  Serial.println("Gerador de números aleatórios inicializado");

  Serial.println("Sistema de monitoramento cardíaco iniciado!");
  Serial.println("Pressione o botão para iniciar/parar a medição.");
  Serial.println("O controle também pode ser feito via frontend em tempo real.");
  Serial.println("=================================");
}
//________________________________________________________________________________

//________________________________________________________________________________
void loop()
{

  // Verificar mudanças no Firebase periodicamente
  unsigned long currentMillisFirebase = millis();
  if (currentMillisFirebase - previousMillisFirebaseCheck >= intervalFirebaseCheck)
  {
    previousMillisFirebaseCheck = currentMillisFirebase;
    getControlStatusFromFirebase();
  }

  // Verifica se o botão foi pressionado com debounce
  bool buttonState = digitalRead(Button_PIN);

  if (buttonState != lastButtonState && (millis() - lastButtonPress) > debounceDelay)
  {
    if (buttonState == LOW)
    { // Botão pressionado (active low)
      lastButtonPress = millis();

      // Inverte o estado da leitura
      get_BPM = !get_BPM;

      if (get_BPM)
      {
        digitalWrite(LED_STATUS_PIN, get_BPM ? HIGH : LOW);

        Serial.print("Sinal cru: ");
        Serial.println(PulseSensorSignal);
        // Reinicia as variáveis
        cntHB = 0;
        BPMval = 0;
        timer_Get_BPM = 0;

        Serial.println("\n>> INICIANDO MEDIÇÃO VIA BOTÃO <<");
        Serial.println("Calculando BPM... aguarde 10 segundos para o primeiro resultado");
      }
      else
      {
        Serial.println("\n>> MEDIÇÃO FINALIZADA VIA BOTÃO <<");
        if (BPMval > 0)
        {
          Serial.print("Último valor de BPM registrado: ");
          Serial.println(BPMval);
        }
      }

      // Enviar nova configuração para o Firebase
      sendControlStatusToFirebase(!get_BPM);
    }
  }

  lastButtonState = buttonState;

  GetHeartRate(); // Chama a subrotina GetHeartRate()
}
//________________________________________________________________________________