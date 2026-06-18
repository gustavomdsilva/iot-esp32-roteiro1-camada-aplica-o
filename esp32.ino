/*
  ============================================================
  AV2 - Sistema de Lâmpada Inteligente
  Práticas Integradas: Camada de Aplicação - SENAI CIMATEC
  Etapa 02 - Com envio ao Supabase + Arduino IoT Cloud
  ESP32 Arduino Core 2.x (Cloud)
  ============================================================
*/

#include "thingProperties.h"
#include <DHT.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── Pinagem ──────────────────────────────────────────────────
#define PIN_LED_R   6
#define PIN_LED_G   7
#define PIN_LED_B   5
#define PIN_DHT     8
#define PIN_BTN     4
#define PIN_BUZZER  18
#define PIN_POT     15
#define PIN_LDR     3

// ── PWM (core 2.x usa ledcAttachPin + ledcSetup) ─────────────
#define PWM_FREQ   5000
#define PWM_RES    8
#define CH_LED_R   0
#define CH_LED_G   1
#define CH_LED_B   2
#define CH_BUZZER  3
#define BUZ_FREQ   2000

// ── Supabase ─────────────────────────────────────────────────
const char* SUPABASE_URL    = "https://zvulzkhnimjgfhqxbbpo.supabase.co/rest/v1/leituras_sensores";
const char* SUPABASE_APIKEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Inp2dWx6a2huaW1qZ2ZocXhiYnBvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODEzMzQ2OTUsImV4cCI6MjA5NjkxMDY5NX0.My2o-ySH9gPPYUHZkUvKSvwkA2J1a5uN1k3SwjhMZsc";

// ── DHT22 ────────────────────────────────────────────────────
DHT dht(PIN_DHT, DHT22);

// ── Limites ──────────────────────────────────────────────────
#define TEMP_MIN  0.0f
#define TEMP_MAX  25.0f
#define LDR_CLARO_THRESHOLD  1900

// ── Média ADC ────────────────────────────────────────────────
#define NUM_AMOSTRAS 16
int lerADC(int pino) {
  long soma = 0;
  for (int i = 0; i < NUM_AMOSTRAS; i++) {
    soma += analogRead(pino);
    delayMicroseconds(100);
  }
  return (int)(soma / NUM_AMOSTRAS);
}

// ── Estado local ─────────────────────────────────────────────
volatile bool btnPendente    = false;
volatile unsigned long ultimoISR = 0;
#define DEBOUNCE_MS 400

bool tempAtiva        = true;
bool detectorAtivo    = true;
bool buzzerAtivo      = true;
bool ultimoEstadoBotao = false;

unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvio   = 0;
#define INTERVALO_MS     2000
#define INTERVALO_ENVIO  10000

float tempLocal = NAN;
float umidLocal = NAN;
int   ldrLocal  = 0;
int   potLocal  = 0;

// ════════════════════════════════════════════════════════════
// ISR – Botão físico
// ════════════════════════════════════════════════════════════
void IRAM_ATTR isrBotao() {
  unsigned long agora = millis();
  if (agora - ultimoISR > DEBOUNCE_MS) {
    btnPendente = true;
    ultimoISR   = agora;
  }
}

// ════════════════════════════════════════════════════════════
// LED RGB  (core 2.x: ledcSetup + ledcAttachPin + ledcWrite)
// ════════════════════════════════════════════════════════════
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(CH_LED_R, r);
  ledcWrite(CH_LED_G, g);
  ledcWrite(CH_LED_B, b);
}
void ledOFF() { setLED(0, 0, 0); }
void setCorPeloPot(int v) {
  if      (v < 682)  setLED(255,   0,   0);
  else if (v < 1365) setLED(255, 100,   0);
  else if (v < 2048) setLED(255, 200, 100);
  else if (v < 2730) setLED(255, 255, 200);
  else if (v < 3413) setLED(  0, 200, 255);
  else               setLED(  0,   0, 255);
}

// ════════════════════════════════════════════════════════════
// Buzzer
// ════════════════════════════════════════════════════════════
void buzzerON()  { ledcWriteTone(CH_BUZZER, BUZ_FREQ); }
void buzzerOFF() { ledcWriteTone(CH_BUZZER, 0); }

// ════════════════════════════════════════════════════════════
// Envio ao Supabase
// ════════════════════════════════════════════════════════════
void enviarSupabase(float temp, float umid, int ldr, int pot, bool botao) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Sem conexão, pulando envio.");
    return;
  }
  HTTPClient http;
  http.begin(SUPABASE_URL);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_APIKEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_APIKEY);
  http.addHeader("Prefer",        "return=minimal");

  String payload = "{";
  payload += "\"temperatura\":"           + String(temp, 2) + ",";
  payload += "\"umidade\":"               + String(umid, 2) + ",";
  payload += "\"luminosidade\":"          + String(ldr)     + ",";
  payload += "\"leitura_potenciometro\":" + String(pot)     + ",";
  payload += "\"botao_pressionado\":"     + String(botao ? "true" : "false");
  payload += "}";

  Serial.print("[SUPABASE] Payload: "); Serial.println(payload);
  int httpCode = http.POST(payload);
  Serial.print("[SUPABASE] HTTP Code: "); Serial.println(httpCode);
  if (httpCode == 201) Serial.println("[SUPABASE] Inserido com sucesso!");
  else                 Serial.println("[SUPABASE] Falha no envio!");
  http.end();
}

// ════════════════════════════════════════════════════════════
// Executa comandos (Serial ou Cloud)
// ════════════════════════════════════════════════════════════
void executarComando(String cmd) {
  cmd.trim();
  Serial.print("[CMD] Executando: "); Serial.println(cmd);

  if (cmd.equalsIgnoreCase("Ligar")) {
    sistemaAtivo = true;

  } else if (cmd.equalsIgnoreCase("Desligar")) {
    sistemaAtivo = false;
    ledOFF();
    buzzerOFF();

  } else if (cmd.equalsIgnoreCase("Vermelho")) {
    setLED(255, 0, 0);
    delay(1000);

  } else if (cmd.equalsIgnoreCase("Amarelo")) {
    setLED(255, 200, 0);
    delay(1000);

  } else if (cmd.equalsIgnoreCase("Azul")) {
    setLED(0, 0, 255);
    delay(1000);

  } else if (cmd.equalsIgnoreCase("Ativar Temperatura")) {
    tempAtiva = true;

  } else if (cmd.equalsIgnoreCase("Desativar Temperatura")) {
    tempAtiva = false;

  } else if (cmd.equalsIgnoreCase("Ativar Detector")) {
    detectorAtivo = true;

  } else if (cmd.equalsIgnoreCase("Desativar Detector")) {
    detectorAtivo = false;

  } else if (cmd.equalsIgnoreCase("Ativar Buzzer")) {
    buzzerAtivo = true;

  } else if (cmd.equalsIgnoreCase("Desativar Buzzer")) {
    buzzerAtivo = false;
    buzzerOFF();

  } else {
    Serial.print("[CMD] Desconhecido: "); Serial.println(cmd);
  }

  // Limpa o campo no Cloud após processar
  comando = "";
}

// ════════════════════════════════════════════════════════════
// CALLBACKS DO ARDUINO CLOUD
// ════════════════════════════════════════════════════════════
void onComandoChange() {
  if (comando.length() > 0) {
    Serial.print("[CLOUD] Comando recebido: "); Serial.println(comando);
    executarComando(comando);
  }
}

void onSistemaAtivoChange() {
  Serial.print("[CLOUD] sistemaAtivo -> ");
  Serial.println(sistemaAtivo ? "ATIVO" : "INATIVO");
  if (!sistemaAtivo) {
    ledOFF();
    buzzerOFF();
  }
}

// Callbacks obrigatórios gerados pelo Cloud (variáveis READ ONLY
// não precisam de lógica aqui, mas devem existir)
void onTemperaturaChange()  {}
void onUmidadeChange()      {}
void onLuminosidadeChange() {}
void onPotenciometroChange(){}

// ════════════════════════════════════════════════════════════
// setup()
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== AV2 - Lampada Inteligente | SENAI CIMATEC ===");

  // PWM – core 2.x
  ledcSetup(CH_LED_R,  PWM_FREQ, PWM_RES);
  ledcSetup(CH_LED_G,  PWM_FREQ, PWM_RES);
  ledcSetup(CH_LED_B,  PWM_FREQ, PWM_RES);
  ledcSetup(CH_BUZZER, PWM_FREQ, PWM_RES);

  ledcAttachPin(PIN_LED_R,  CH_LED_R);
  ledcAttachPin(PIN_LED_G,  CH_LED_G);
  ledcAttachPin(PIN_LED_B,  CH_LED_B);
  ledcAttachPin(PIN_BUZZER, CH_BUZZER);

  // Botão físico
  pinMode(PIN_BTN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), isrBotao, RISING);

  dht.begin();
  ledOFF();
  buzzerOFF();

  // Arduino IoT Cloud
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();
}

// ════════════════════════════════════════════════════════════
// loop()
// ════════════════════════════════════════════════════════════
void loop() {
  ArduinoCloud.update();

  // 1. Botão físico
  if (btnPendente) {
    btnPendente        = false;
    sistemaAtivo       = !sistemaAtivo;
    ultimoEstadoBotao  = !ultimoEstadoBotao;
    Serial.print("[BTN] Sistema -> ");
    Serial.println(sistemaAtivo ? "ATIVO" : "INATIVO");
  }

  // 2. Comandos Serial (debug local)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    executarComando(cmd);
  }

  // 3. Leituras periódicas
  unsigned long agora = millis();
  if (agora - ultimaLeitura < INTERVALO_MS) return;
  ultimaLeitura = agora;

  // 3a. Temperatura / Umidade
  bool perigo = false;
  if (tempAtiva) {
    float t = dht.readTemperature();
    float u = dht.readHumidity();
    if (isnan(t)) {
      Serial.println("[TEMP] Falha na leitura do DHT22!");
    } else {
      tempLocal    = t;
      umidLocal    = u;
      Temperatura  = tempLocal;   // atualiza Cloud (maiúsculo!)
      Umidade      = umidLocal;   // atualiza Cloud
      Serial.printf("[TEMP] %.1f C  |  [UMID] %.1f %%\n", tempLocal, umidLocal);
      if (tempLocal < TEMP_MIN || tempLocal > TEMP_MAX) {
        perigo = true;
        Serial.println("[TEMP] PERIGO! Fora do limite!");
      }
    }
  }

  // 3b. LDR
  bool ambienteClaro = false;
  ldrLocal     = lerADC(PIN_LDR);
  Luminosidade = ldrLocal;        // atualiza Cloud (maiúsculo!)
  if (detectorAtivo) {
    ambienteClaro = (ldrLocal > LDR_CLARO_THRESHOLD);
    Serial.printf("[LDR] ADC: %d -> %s\n", ldrLocal, ambienteClaro ? "Claro" : "Escuro");
  }

  // 3c. Potenciômetro
  potLocal     = lerADC(PIN_POT);
  Potenciometro = potLocal;       // atualiza Cloud
  Serial.printf("[POT] ADC: %d\n", potLocal);

  // 4. Lógica LED
  if (!sistemaAtivo)                       { ledOFF(); Serial.println("[LED] OFF (sistema inativo)"); }
  else if (perigo)                         { ledOFF(); Serial.println("[LED] OFF (temp fora do limite)"); }
  else if (detectorAtivo && ambienteClaro) { ledOFF(); Serial.println("[LED] OFF (ambiente claro)"); }
  else                                     { setCorPeloPot(potLocal); Serial.println("[LED] ON"); }

  // 5. Buzzer
  if (buzzerAtivo && sistemaAtivo && perigo) { buzzerON();  Serial.println("[BUZZER] TOCANDO"); }
  else                                        { buzzerOFF(); }

  // 6. Envio ao Supabase
  if (agora - ultimoEnvio >= INTERVALO_ENVIO) {
    ultimoEnvio = agora;
    if (!isnan(tempLocal)) {
      enviarSupabase(tempLocal, umidLocal, ldrLocal, potLocal, ultimoEstadoBotao);
    } else {
      Serial.println("[SUPABASE] Pulando envio (temperatura invalida).");
    }
  }

  Serial.print("[SISTEMA] ");
  Serial.println(sistemaAtivo ? "ATIVO" : "INATIVO");
  Serial.println("-------------------------------------------------");
}