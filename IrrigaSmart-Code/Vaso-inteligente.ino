
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <time.h>
#include <LittleFS.h>
#include "webpage.h"


// CONFIGURAÇÕES WiFi

const char* WIFI_SSID = "Larissa-Pernambuco_Telecom";
const char* WIFI_PASS = "suasenha";


// NTP  (Recife = UTC-3, sem horário de verão)

const long  GMT_OFFSET_SEC     = -3L * 3600L;
const int   DAYLIGHT_OFFSET_SEC = 0;


// PINOS

const int PINO_SENSOR_SOLO = 34;
const int PINO_RELE        = 26;
const int PINO_DHT         = 33;


// CALIBRAÇÃO DO SENSOR DE SOLO

int VALOR_SECO  = 2800;
int VALOR_UMIDO = 1400;


// PARÂMETROS DE IRRIGAÇÃO

const float UMIDADE_MINIMA          = 40.0f;
const int   TEMPO_BOMBA_MS          = 1000;
const int   ESPERA_DEPOIS_BOMBA_MS  = 10000;


// REGRAS DHT22

const float TEMPERATURA_MAXIMA  = 35.0f;
const float TEMPERATURA_CALOR   = 30.0f;
const float UMIDADE_AR_HUMIDO   = 80.0f;
const float REDUCAO_HUMIDADE    = 0.70f;


// ESTIMATIVA DE CONSUMO

const float VAZAO_BOMBA_L_MIN        = 1.6f;
const float IRRIGACAO_MANUAL_MIN_DIA = 40.0f;


#define DHTTYPE DHT22
DHT dht(PINO_DHT, DHTTYPE);

WebServer server(80);



struct DadoHistorico {
  char  horario[6];
  float temperatura;
  float umidadeAr;
  float umidadeSolo;
};

struct EventoIrrigacao {
  char          horario[6];
  unsigned long duracao_ms;
  float         umidadeAntes;
  float         umidadeDepois;
  bool          automatica;
};


const int MAX_HISTORICO = 144;
const int MAX_EVENTOS   = 50;

DadoHistorico   historico[MAX_HISTORICO];
int historicoIndex = 0;
int historicoCount = 0;

EventoIrrigacao eventos[MAX_EVENTOS];
int eventoIndex = 0;
int eventoCount = 0;


unsigned long totalLeituras       = 0;
unsigned long totalAcionamentos   = 0;
unsigned long tempoTotalBombaMs   = 0;
unsigned long inicioMonitoramento = 0;

bool          bombaLigada       = false;
bool          bombaManual       = false;
unsigned long tempoInicioBomba  = 0;
unsigned long fimBombaMs        = 0;
unsigned long ultimaLeitura     = 0;
unsigned long ultimoRegistro    = 0;

const unsigned long PERIODO_LEITURA_MS  = 1000UL;
const unsigned long PERIODO_REGISTRO_MS = 600000UL;

float temperatura       = 25.0f;
float umidadeAr         = 50.0f;
float umidadeSolo       = 0.0f;
float limiar            = UMIDADE_MINIMA;
float umidadeAntesBomba = 0.0f;

bool bloquadoPorTemperatura = false;
bool reducaoPorHumidade     = false;


int  falhasDHT_consecutivas   = 0;
bool dhtOk                    = false;   
const int MAX_FALHAS_DHT      = 3;       



String getHorario() {
  struct tm ti;
  if (!getLocalTime(&ti)) {
    unsigned long s = millis() / 1000UL;
    char buf[6];
    snprintf(buf, 6, "%02lu:%02lu", (s / 3600UL) % 24UL, (s / 60UL) % 60UL);
    return String(buf);
  }
  char buf[6];
  strftime(buf, 6, "%H:%M", &ti);
  return String(buf);
}



void salvarHistorico() {
  File f = LittleFS.open("/historico.csv", "w");
  if (!f) { Serial.println(" Erro ao abrir historico.csv"); return; }

  f.println("horario,temperatura,umidadeAr,umidadeSolo");
  int start = (historicoCount < MAX_HISTORICO) ? 0 : historicoIndex;
  for (int i = 0; i < historicoCount; i++) {
    int idx = (start + i) % MAX_HISTORICO;
    f.printf("%s,%.1f,%.1f,%.1f\n",
             historico[idx].horario,
             historico[idx].temperatura,
             historico[idx].umidadeAr,
             historico[idx].umidadeSolo);
  }
  f.close();
  Serial.println("💾 Histórico salvo em CSV.");
}

void carregarHistorico() {
  if (!LittleFS.exists("/historico.csv")) return;
  File f = LittleFS.open("/historico.csv", "r");
  if (!f) return;

  f.readStringUntil('\n'); 
  historicoCount = 0;
  historicoIndex = 0;

  while (f.available() && historicoCount < MAX_HISTORICO) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    int c1 = linha.indexOf(',');
    int c2 = linha.indexOf(',', c1 + 1);
    int c3 = linha.indexOf(',', c2 + 1);

    String hor = linha.substring(0, c1);
    strncpy(historico[historicoCount].horario, hor.c_str(), 5);
    historico[historicoCount].horario[5]  = '\0';
    historico[historicoCount].temperatura = linha.substring(c1 + 1, c2).toFloat();
    historico[historicoCount].umidadeAr   = linha.substring(c2 + 1, c3).toFloat();
    historico[historicoCount].umidadeSolo = linha.substring(c3 + 1).toFloat();
    historicoCount++;
  }
  historicoIndex = historicoCount % MAX_HISTORICO;
  f.close();
  Serial.printf(" Histórico carregado: %d registros.\n", historicoCount);
}

void salvarEventos() {
  File f = LittleFS.open("/eventos.csv", "w");
  if (!f) { Serial.println(" Erro ao abrir eventos.csv"); return; }

  f.println("horario,duracao_s,umidadeAntes,umidadeDepois,automatica");
  int start = (eventoCount < MAX_EVENTOS) ? 0 : eventoIndex;
  for (int i = 0; i < eventoCount; i++) {
    int idx = (start + i) % MAX_EVENTOS;
    f.printf("%s,%lu,%.1f,%.1f,%s\n",
             eventos[idx].horario,
             eventos[idx].duracao_ms / 1000UL,
             eventos[idx].umidadeAntes,
             eventos[idx].umidadeDepois,
             eventos[idx].automatica ? "sim" : "nao");
  }
  f.close();
  Serial.println(" Eventos salvos em CSV.");
}

void carregarEventos() {
  if (!LittleFS.exists("/eventos.csv")) return;
  File f = LittleFS.open("/eventos.csv", "r");
  if (!f) return;

  f.readStringUntil('\n'); 
  eventoCount = 0;
  eventoIndex = 0;

  while (f.available() && eventoCount < MAX_EVENTOS) {
    String linha = f.readStringUntil('\n');
    linha.trim();
    if (linha.length() == 0) continue;

    int c1 = linha.indexOf(',');
    int c2 = linha.indexOf(',', c1 + 1);
    int c3 = linha.indexOf(',', c2 + 1);
    int c4 = linha.indexOf(',', c3 + 1);

    String hor = linha.substring(0, c1);
    strncpy(eventos[eventoCount].horario, hor.c_str(), 5);
    eventos[eventoCount].horario[5]     = '\0';
    eventos[eventoCount].duracao_ms     = linha.substring(c1 + 1, c2).toInt() * 1000UL;
    eventos[eventoCount].umidadeAntes   = linha.substring(c2 + 1, c3).toFloat();
    eventos[eventoCount].umidadeDepois  = linha.substring(c3 + 1, c4).toFloat();
    eventos[eventoCount].automatica     = linha.substring(c4 + 1).indexOf("sim") >= 0;
    eventoCount++;
  }
  eventoIndex = eventoCount % MAX_EVENTOS;
  f.close();
  Serial.printf(" Eventos carregados: %d registros.\n", eventoCount);
}

void salvarEstatisticas() {
  File f = LittleFS.open("/stats.csv", "w");
  if (!f) return;
  f.println("leituras,acionamentos,tempoBombaMs");
  f.printf("%lu,%lu,%lu\n", totalLeituras, totalAcionamentos, tempoTotalBombaMs);
  f.close();
}

void carregarEstatisticas() {
  if (!LittleFS.exists("/stats.csv")) return;
  File f = LittleFS.open("/stats.csv", "r");
  if (!f) return;

  f.readStringUntil('\n'); 
  String linha = f.readStringUntil('\n');
  linha.trim();

  int c1 = linha.indexOf(',');
  int c2 = linha.indexOf(',', c1 + 1);

  totalLeituras     = linha.substring(0, c1).toInt();
  totalAcionamentos = linha.substring(c1 + 1, c2).toInt();
  tempoTotalBombaMs = linha.substring(c2 + 1).toInt();

  f.close();
  Serial.printf(" Stats carregados: %lu leituras, %lu acionamentos.\n",
                totalLeituras, totalAcionamentos);
}



void registrarHistorico() {
  String h = getHorario();
  strncpy(historico[historicoIndex].horario, h.c_str(), 5);
  historico[historicoIndex].horario[5]  = '\0';
  historico[historicoIndex].temperatura = temperatura;
  historico[historicoIndex].umidadeAr   = umidadeAr;
  historico[historicoIndex].umidadeSolo = umidadeSolo;

  historicoIndex = (historicoIndex + 1) % MAX_HISTORICO;
  if (historicoCount < MAX_HISTORICO) historicoCount++;
  salvarHistorico();
}

void registrarEventoIrrigacao(unsigned long dur_ms, float antes, float depois, bool autoAcion) {
  String h = getHorario();
  strncpy(eventos[eventoIndex].horario, h.c_str(), 5);
  eventos[eventoIndex].horario[5]    = '\0';
  eventos[eventoIndex].duracao_ms    = dur_ms;
  eventos[eventoIndex].umidadeAntes  = antes;
  eventos[eventoIndex].umidadeDepois = depois;
  eventos[eventoIndex].automatica    = autoAcion;

  eventoIndex = (eventoIndex + 1) % MAX_EVENTOS;
  if (eventoCount < MAX_EVENTOS) eventoCount++;

  totalAcionamentos++;
  tempoTotalBombaMs += dur_ms;
  salvarEventos();
  salvarEstatisticas();
}



void handleIndex() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleDados() {
  String json;
  json.reserve(300);
  json += "{";
  json += "\"temperatura\":"  + String(temperatura,   1) + ",";
  json += "\"umidadeAr\":"    + String(umidadeAr,     1) + ",";
  json += "\"umidadeSolo\":"  + String(umidadeSolo,   1) + ",";
  json += "\"limiar\":"       + String(limiar,         1) + ",";
  json += "\"bombaLigada\":"  + String(bombaLigada ? "true" : "false") + ",";
  json += "\"bloquadoPorTemperatura\":" + String(bloquadoPorTemperatura ? "true" : "false") + ",";
  json += "\"reducaoPorHumidade\":"     + String(reducaoPorHumidade     ? "true" : "false") + ",";
  json += "\"valorSeco\":"    + String(VALOR_SECO)   + ",";
  json += "\"valorUmido\":"   + String(VALOR_UMIDO)  + ",";
  json += "\"umidadeMinima\":" + String(UMIDADE_MINIMA) + ",";
  json += "\"dhtOk\":"        + String(dhtOk ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleHistorico() {
  String json;
  json.reserve(historicoCount * 70 + 30);
  json += "{\"count\":" + String(historicoCount) + ",\"readings\":[";

  int start = (historicoCount < MAX_HISTORICO) ? 0 : historicoIndex;
  for (int i = 0; i < historicoCount; i++) {
    int idx = (start + i) % MAX_HISTORICO;
    if (i > 0) json += ",";
    json += "{\"time\":\"" + String(historico[idx].horario) + "\"";
    json += ",\"temperatura\":"  + String(historico[idx].temperatura, 1);
    json += ",\"umidadeAr\":"    + String(historico[idx].umidadeAr,   1);
    json += ",\"umidadeSolo\":"  + String(historico[idx].umidadeSolo, 1);
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleIrrigacao() {
  String json;
  json.reserve(eventoCount * 90 + 30);
  json += "{\"count\":" + String(eventoCount) + ",\"events\":[";

  int start = (eventoCount < MAX_EVENTOS) ? 0 : eventoIndex;
  for (int i = 0; i < eventoCount; i++) {
    int idx = (start + i) % MAX_EVENTOS;
    if (i > 0) json += ",";
    json += "{\"time\":\"" + String(eventos[idx].horario) + "\"";
    json += ",\"duracao_s\":"    + String(eventos[idx].duracao_ms / 1000UL);
    json += ",\"umidadeAntes\":" + String(eventos[idx].umidadeAntes,  1);
    json += ",\"umidadeDepois\":"+ String(eventos[idx].umidadeDepois, 1);
    json += ",\"automatica\":"   + String(eventos[idx].automatica ? "true" : "false");
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleEstatisticas() {
  unsigned long decMs = millis() - inicioMonitoramento;
  float diasDecorridos = decMs / 86400000.0f;
  if (diasDecorridos < 0.001f) diasDecorridos = 0.001f;

  float minutosInt    = tempoTotalBombaMs / 60000.0f;
  float minutosManual = IRRIGACAO_MANUAL_MIN_DIA * diasDecorridos;
  float litrosInt     = minutosInt    * VAZAO_BOMBA_L_MIN;
  float litrosManual  = minutosManual * VAZAO_BOMBA_L_MIN;
  float economia      = litrosManual - litrosInt;
  if (economia < 0) economia = 0;

  float eficiencia = 0.0f;
  if (minutosManual > 0.0f) {
    eficiencia = (1.0f - minutosInt / minutosManual) * 100.0f;
    if (eficiencia < 0.0f)   eficiencia = 0.0f;
    if (eficiencia > 100.0f) eficiencia = 100.0f;
  }

  float tempoMedio = (totalAcionamentos > 0)
    ? (tempoTotalBombaMs / 1000.0f / (float)totalAcionamentos)
    : 0.0f;

  String json;
  json.reserve(300);
  json += "{";
  json += "\"diasMonitorados\":"    + String(diasDecorridos,    2) + ",";
  json += "\"leituras\":"           + String(totalLeituras)        + ",";
  json += "\"acionamentos\":"       + String(totalAcionamentos)    + ",";
  json += "\"tempoMedioIrrigacao\":" + String(tempoMedio,       1) + ",";
  json += "\"minutosInteligente\":" + String(minutosInt,         2) + ",";
  json += "\"minutosManual\":"      + String(minutosManual,      1) + ",";
  json += "\"litrosInteligente\":"  + String(litrosInt,          1) + ",";
  json += "\"litrosManual\":"       + String(litrosManual,       1) + ",";
  json += "\"economiaLitros\":"     + String(economia,           1) + ",";
  json += "\"eficiencia\":"         + String(eficiencia,         1);
  json += "}";
  server.send(200, "application/json", json);
}

void handleLigarBomba() {
  if (!bloquadoPorTemperatura && !bombaLigada) {
    umidadeAntesBomba = umidadeSolo;
    bombaManual       = true;
    digitalWrite(PINO_RELE, HIGH);
    bombaLigada      = true;
    tempoInicioBomba = millis();
    Serial.println("→ Bomba ligada MANUALMENTE via painel");
  }
  handleDados();
}

void handleDesligarBomba() {
  if (bombaLigada) {
    unsigned long dur = millis() - tempoInicioBomba;
    digitalWrite(PINO_RELE, LOW);
    bombaLigada = false;
    registrarEventoIrrigacao(dur, umidadeAntesBomba, umidadeSolo, false);
    fimBombaMs  = millis();
    Serial.println("→ Bomba desligada MANUALMENTE via painel");
  }
  handleDados();
}


void handleDownloadHistorico() {
  if (!LittleFS.exists("/historico.csv")) {
    server.send(404, "text/plain", "Arquivo nao encontrado. Aguarde o primeiro registro (10 min).");
    return;
  }
  File f = LittleFS.open("/historico.csv", "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"historico.csv\"");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleDownloadEventos() {
  if (!LittleFS.exists("/eventos.csv")) {
    server.send(404, "text/plain", "Arquivo nao encontrado. Aguarde a primeira irrigacao.");
    return;
  }
  File f = LittleFS.open("/eventos.csv", "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"eventos.csv\"");
  server.streamFile(f, "text/csv");
  f.close();
}

void handleDownloadStats() {
  if (!LittleFS.exists("/stats.csv")) {
    server.send(404, "text/plain", "Arquivo nao encontrado.");
    return;
  }
  File f = LittleFS.open("/stats.csv", "r");
  server.sendHeader("Content-Disposition", "attachment; filename=\"stats.csv\"");
  server.streamFile(f, "text/csv");
  f.close();
}



void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PINO_RELE, OUTPUT);
  digitalWrite(PINO_RELE, LOW);
  pinMode(PINO_DHT, INPUT_PULLUP);
  dht.begin();

  
  if (!LittleFS.begin(true)) {
    Serial.println(" Falha ao montar LittleFS! Dados não serão persistidos.");
  } else {
    Serial.println(" LittleFS montado.");
    carregarHistorico();
    carregarEventos();
    carregarEstatisticas();
  }

  
  Serial.println("\nConectando ao WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n WiFi conectado!");
  Serial.print("📡 IP: ");
  Serial.println(WiFi.localIP());

  
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "a.ntp.br");
  Serial.println(" Sincronizando horário NTP...");
  struct tm ti;
  if (getLocalTime(&ti)) {
    char buf[20];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &ti);
    Serial.printf(" Horário: %s\n", buf);
  } else {
    Serial.println("  NTP não disponível, usando tempo relativo.");
  }

  inicioMonitoramento = millis();
  ultimoRegistro      = millis();

  
  server.on("/",                   handleIndex);
  server.on("/api/dados",          handleDados);
  server.on("/api/historico",      handleHistorico);
  server.on("/api/irrigacao",      handleIrrigacao);
  server.on("/api/estatisticas",   handleEstatisticas);
  server.on("/api/bomba/ligar",    handleLigarBomba);
  server.on("/api/bomba/desligar", handleDesligarBomba);
  server.on("/download/historico", handleDownloadHistorico);
  server.on("/download/eventos",   handleDownloadEventos);
  server.on("/download/stats",     handleDownloadStats);

  server.begin();
  Serial.println(" WebServer iniciado — porta 80");
  Serial.println("=========================================");
}



void loop() {
  server.handleClient();
  unsigned long agora = millis();

  
  if (bombaLigada) {
    if (agora - tempoInicioBomba >= (unsigned long)TEMPO_BOMBA_MS) {
      digitalWrite(PINO_RELE, LOW);
      unsigned long dur = agora - tempoInicioBomba;
      bombaLigada       = false;
      registrarEventoIrrigacao(dur, umidadeAntesBomba, umidadeSolo, !bombaManual);
      fimBombaMs  = agora;
      bombaManual = false;
      Serial.printf("→ Bomba desligada automaticamente (%.1f s)\n", dur / 1000.0f);
    }
    return;
  }

  
  if (fimBombaMs > 0 && (agora - fimBombaMs) < (unsigned long)ESPERA_DEPOIS_BOMBA_MS) return;

  
  if (agora - ultimaLeitura < PERIODO_LEITURA_MS) return;
  ultimaLeitura = agora;
  totalLeituras++;

  
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h)) {
    temperatura = t;
    umidadeAr   = h;
    falhasDHT_consecutivas = 0;
    dhtOk = true;
  } else {
    falhasDHT_consecutivas++;
    if (falhasDHT_consecutivas >= MAX_FALHAS_DHT) {
      dhtOk = false;
      Serial.println("  DHT22 sem resposta — sensor pode estar desconectado.");
    }
  }

  int adcVal  = analogRead(PINO_SENSOR_SOLO);
  umidadeSolo = 100.0f * (float)(VALOR_SECO - adcVal) / (float)(VALOR_SECO - VALOR_UMIDO);
  umidadeSolo = constrain(umidadeSolo, 0.0f, 100.0f);

 
  limiar = UMIDADE_MINIMA;
  if (temperatura > TEMPERATURA_CALOR) limiar *= 1.10f;
  if (umidadeAr   > UMIDADE_AR_HUMIDO) limiar *= REDUCAO_HUMIDADE;

  bloquadoPorTemperatura = (temperatura > TEMPERATURA_MAXIMA);
  reducaoPorHumidade     = (umidadeAr   > UMIDADE_AR_HUMIDO);

  
  if (agora - ultimoRegistro >= PERIODO_REGISTRO_MS) {
    registrarHistorico();
    ultimoRegistro = agora;
    Serial.printf("[HIST] Temp:%.1f°C  Ar:%.1f%%  Solo:%.1f%%\n",
                  temperatura, umidadeAr, umidadeSolo);
  }

  
  if (!bloquadoPorTemperatura && umidadeSolo < limiar) {
    Serial.printf("→ Solo SECO (%.1f%% < %.1f%%)! Acionando bomba...\n", umidadeSolo, limiar);
    umidadeAntesBomba = umidadeSolo;
    bombaManual       = false;
    digitalWrite(PINO_RELE, HIGH);
    bombaLigada      = true;
    tempoInicioBomba = agora;
  }

  
  Serial.println("──────────────────────────────────────");
  Serial.printf("  Temp:      %.1f °C\n",  temperatura);
  Serial.printf("  Umid. Ar: %.1f %%\n",   umidadeAr);
  Serial.printf("  Solo:     %.1f %%  (limiar: %.1f%%)\n", umidadeSolo, limiar);
  Serial.printf("  Bomba:    %s\n",         bombaLigada ? "LIGADA" : "desligada");
  Serial.printf("  Leituras: %lu | Acionamentos: %lu\n", totalLeituras, totalAcionamentos);
  if (bloquadoPorTemperatura) Serial.println(" BLOQUEADO: Temperatura crítica!");
  if (reducaoPorHumidade)     Serial.println(" REDUÇÃO:  Ar muito úmido!");
}
