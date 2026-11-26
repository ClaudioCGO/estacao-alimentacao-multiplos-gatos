
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <Preferences.h>

// 1. WIFI E RELOGIO
const char* WIFI_SSID = "SUA_REDE";
const char* WIFI_PASSWORD = "SUA_SENHA";
const long GMT_OFFSET = -10800;
const int DAYLIGHT_OFFSET = 0;
const char* NTP_SERVER = "pool.ntp.org";

bool horarioSincronizado = false;
unsigned long ultimaTentativaWiFi = 0, ultimaTentativaNTP = 0;

// 2. PINOS
#define RFID_RX 16
#define SERVO_PIN 10
#define HX711_DT 4
#define HX711_SCK 5

// 3. OBJETOS
HardwareSerial RFID(1);
Servo servo;
HX711 balanca;
Preferences memoria;
WebServer servidor(80);

bool sistemaArquivosPronto = false, servidorIniciado = false;

// 4. SERVO
const int SERVO_FECHADO = 0, SERVO_ABERTO = 30;
float GRAMAS_POR_SEGUNDO = 3.5;
const int TEMPO_MINIMO_ABERTURA = 50, TEMPO_MAXIMO_ABERTURA = 2000;
const int MAX_PULSOS = 20;

// 5. BALANCA
float FATOR_CALIBRACAO = 176.222;
const bool BALANCA_CALIBRADA = true, TARAR_NA_INICIALIZACAO = false;
const float TOLERANCIA_ESTABILIDADE = 0.3;
const unsigned long TEMPO_MAX_ESTABILIZACAO = 4000;

// 6. GATOS
struct Gato {
  const char* nome;
  const char* codigoRFID;
  float limites[3];
  float consumidoPeriodo;
};

Gato gatos[] = {
  {"Gato 1", "6500823530", {14.0, 14.0, 15.0}, 0},
  {"Gato 2", "6700208506", {17.0, 17.0, 18.0}, 0}
};

const int TOTAL_GATOS = sizeof(gatos) / sizeof(gatos[0]);

// 7. PERIODOS E TEMPOS
float obterLimitePeriodo(int indiceGato, int periodo) {
  if (indiceGato < 0 || indiceGato >= TOTAL_GATOS || periodo < 0 || periodo > 2) return 0;
  return gatos[indiceGato].limites[periodo];
}

int obterPeriodo(int hora) {
  if (hora >= 6 && hora < 12) return 0;
  if (hora >= 12 && hora < 18) return 1;
  return 2;
}

const unsigned long TEMPO_CONSUMO = 60000;
const unsigned long TEMPO_AUSENCIA_RFID = 5000;

// 8. CONTROLE DO PERIODO E AVISOS
String periodoAtual = "";
int periodoAtualNumero = -1;

const unsigned long INTERVALO_REGISTRO_NEGACAO = 60000;
const unsigned long INTERVALO_AVISO_SERIAL = 10000;

unsigned long ultimaNegacao[TOTAL_GATOS] = {0};
unsigned long ultimoAvisoTagDesconhecida = 0, ultimoAvisoBloqueio = 0;

bool intervaloPassou(unsigned long &ultimo, unsigned long intervalo) {
  unsigned long agora = millis();
  if (ultimo != 0 && agora - ultimo < intervalo) return false;
  ultimo = (agora == 0) ? 1 : agora;
  return true;
}

// 9. RELATORIO TXT
void registrarRelatorio(String texto) {
  if (!sistemaArquivosPronto) return;

  File arquivo = LittleFS.open("/relatorio.txt", "a");
  if (!arquivo) {
    Serial.println("Erro ao abrir relatorio.txt");
    return;
  }

  struct tm tempo;
  char dataHora[25];

  if (obterHorario(tempo)) strftime(dataHora, sizeof(dataHora), "%d/%m/%Y %H:%M:%S", &tempo);
  else strcpy(dataHora, "Horario indisponivel");

  int inicio = 0;
  while (inicio < texto.length()) {
    int fim = texto.indexOf('\n', inicio);
    if (fim < 0) fim = texto.length();

    String linha = texto.substring(inicio, fim);
    if (linha.length() > 0) {
      arquivo.print("[");
      arquivo.print(dataHora);
      arquivo.print("] ");
      arquivo.println(linha);
    } else arquivo.println();

    inicio = fim + 1;
  }
  arquivo.close();
}

void iniciarServidor() {
  if (servidorIniciado || WiFi.status() != WL_CONNECTED) return;

  servidor.on("/", HTTP_GET, []() {
    String pagina = "<html><head><meta charset='UTF-8'></head><body><h2>Estacao de Alimentacao</h2><p><a href='/relatorio.txt'>Baixar relatorio.txt</a></p></body></html>";
    servidor.send(200, "text/html", pagina);
  });

  servidor.on("/relatorio.txt", HTTP_GET, []() {
    if (!sistemaArquivosPronto || !LittleFS.exists("/relatorio.txt")) {
      servidor.send(404, "text/plain", "Relatorio ainda nao existe.");
      return;
    }

    File arquivo = LittleFS.open("/relatorio.txt", "r");
    servidor.sendHeader("Content-Disposition", "attachment; filename=relatorio.txt");
    servidor.streamFile(arquivo, "text/plain");
    arquivo.close();
  });

  servidor.begin();
  servidorIniciado = true;
  Serial.println("Servidor de relatorio iniciado.");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// 10. SINCRONIZACAO NTP
void iniciarNTP() {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("Sincronizando horario...");
  configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER, "time.google.com");

  unsigned long inicio = millis();
  while (millis() - inicio < 12000) {
    if (time(nullptr) > 1700000000) {
      horarioSincronizado = true;
      Serial.println("Horario sincronizado!");
      registrarRelatorio("Horario sincronizado.");
      return;
    }
    delay(250);
  }
  Serial.println("Falha na sincronizacao NTP.");
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Conectando ao Wi-Fi...");

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) delay(500);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi conectado!");
    iniciarNTP();
    iniciarServidor();
    ultimaTentativaNTP = millis();
  } else Serial.println("Wi-Fi indisponivel.");
}

void manterWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    iniciarServidor();
    if (!horarioSincronizado && millis() - ultimaTentativaNTP >= 30000) {
      ultimaTentativaNTP = millis();
      iniciarNTP();
    }
    return;
  }

  if (millis() - ultimaTentativaWiFi >= 30000) {
    ultimaTentativaWiFi = millis();
    Serial.println("Tentando reconectar Wi-Fi...");
    WiFi.reconnect();
  }
}

// 11. LEITURA DO HORARIO
bool obterHorario(struct tm &tempo) {
  if (!horarioSincronizado) return false;
  return getLocalTime(&tempo, 1000);
}

String obterData() {
  struct tm tempo;
  if (!obterHorario(tempo)) return "";

  char buffer[11];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tempo);
  return String(buffer);
}

// 12. IDENTIFICACAO DO PERIODO
String obterChavePeriodo(struct tm tempo, int periodo) {
  if (periodo == 2 && tempo.tm_hour < 6) tempo.tm_mday--;
  mktime(&tempo);

  char data[11];
  strftime(data, sizeof(data), "%Y-%m-%d", &tempo);
  return String(data) + "_P" + String(periodo);
}

String obterChavePeriodoAtual() {
  if (!horarioSincronizado) return "";

  time_t agora = time(nullptr);
  struct tm tempo;
  localtime_r(&agora, &tempo);
  return obterChavePeriodo(tempo, obterPeriodo(tempo.tm_hour));
}

// 13. MEMORIA INTERNA
void carregarConsumos() {
  for (int i = 0; i < TOTAL_GATOS; i++) {
    char chave[10];
    sprintf(chave, "gato%d", i);
    gatos[i].consumidoPeriodo = memoria.getFloat(chave, 0);
  }
}

void salvarConsumo(int indice) {
  char chave[10];
  sprintf(chave, "gato%d", indice);
  memoria.putFloat(chave, gatos[indice].consumidoPeriodo);
}

// 14. RESET DOS PERIODOS
void verificarMudancaPeriodo() {
  struct tm tempo;
  if (!obterHorario(tempo)) return;

  int novoPeriodo = obterPeriodo(tempo.tm_hour);
  String novaChave = obterChavePeriodo(tempo, novoPeriodo);

  if (novaChave == periodoAtual) {
    periodoAtualNumero = novoPeriodo;
    return;
  }

  periodoAtual = novaChave;
  periodoAtualNumero = novoPeriodo;

  for (int i = 0; i < TOTAL_GATOS; i++) {
    gatos[i].consumidoPeriodo = 0;
    salvarConsumo(i);
  }

  memoria.putString("periodo", periodoAtual);

  String nomePeriodo;
  if (novoPeriodo == 0) nomePeriodo = "MANHA";
  if (novoPeriodo == 1) nomePeriodo = "TARDE";
  if (novoPeriodo == 2) nomePeriodo = "NOITE";

  Serial.print("Novo periodo: ");
  Serial.println(nomePeriodo);
  registrarRelatorio("Inicio do periodo " + nomePeriodo);
}

// 15. LEITOR RFID
const bool VALIDAR_CHECKSUM_RFID = true;

uint8_t hexValor(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

String lerRFID() {
  static bool recebendo = false;
  static char dados[13];
  static int posicao = 0;

  while (RFID.available()) {
    char c = RFID.read();

    if (c == 0x02) {
      recebendo = true;
      posicao = 0;
      continue;
    }

    if (!recebendo) continue;

    if (c == 0x03) {
      recebendo = false;

      if (posicao == 12) {
        dados[12] = '\0';

        if (VALIDAR_CHECKSUM_RFID) {
          uint8_t calculado = 0;
          for (int i = 0; i < 5; i++)
            calculado ^= (hexValor(dados[2 * i]) << 4) | hexValor(dados[2 * i + 1]);

          uint8_t recebido = (hexValor(dados[10]) << 4) | hexValor(dados[11]);

          if (calculado != recebido) {
            Serial.println("RFID: checksum invalido, leitura descartada.");
            posicao = 0;
            continue;
          }
        }

        String codigo = "";
        for (int i = 0; i < 10; i++) codigo += dados[i];

        codigo.toUpperCase();
        posicao = 0;
        return codigo;
      }

      posicao = 0;
      continue;
    }

    if (!isxdigit((unsigned char)c)) {
      recebendo = false;
      posicao = 0;
      continue;
    }

    if (posicao < 12) dados[posicao++] = c;
    else {
      recebendo = false;
      posicao = 0;
    }
  }
  return "";
}

// 16. IDENTIFICACAO DO GATO
int identificarGato(String codigo) {
  codigo.toUpperCase();

  for (int i = 0; i < TOTAL_GATOS; i++) {
    if (strlen(gatos[i].codigoRFID) == 0) continue;

    String cadastrado = String(gatos[i].codigoRFID);
    cadastrado.toUpperCase();
    if (codigo == cadastrado) return i;
  }
  return -1;
}

// 17. LEITURA DO PESO
float lerPeso() {
  if (!balanca.is_ready()) {
    Serial.println("Erro: HX711 indisponivel.");
    return NAN;
  }

  float peso = balanca.get_units(5);
  if (!isfinite(peso) || peso < -2) {
    Serial.println("Erro: leitura invalida.");
    return NAN;
  }

  if (peso < 0) peso = 0;
  return peso;
}

float lerPesoEstavel(unsigned long tempoMaximo) {
  float anterior = lerPeso();
  if (!isfinite(anterior)) return NAN;

  unsigned long inicio = millis();
  while (millis() - inicio < tempoMaximo) {
    delay(300);
    float atual = lerPeso();
    if (!isfinite(atual)) return NAN;

    if (fabs(atual - anterior) <= TOLERANCIA_ESTABILIDADE) return atual;
    anterior = atual;
  }

  Serial.println("Aviso: peso nao estabilizou no tempo maximo.");
  return anterior;
}

// 18. CONTROLE DO SERVO
void abrirComporta(int tempo) {
  servo.write(SERVO_ABERTO);
  delay(tempo);
  servo.write(SERVO_FECHADO);
  delay(500);
}

// 19. LIBERACAO DE RACAO
float liberarRacao(float pesoAtual, float quantidadeNecessaria) {
  if (GRAMAS_POR_SEGUNDO <= 0) {
    Serial.println("Calibracao do servo pendente.");
    return NAN;
  }

  float pesoDesejado = pesoAtual + quantidadeNecessaria;
  int pulsos = 0, semProgresso = 0;

  while (pesoAtual < pesoDesejado - 0.5 && pulsos < MAX_PULSOS) {
    float anterior = pesoAtual;
    float falta = pesoDesejado - pesoAtual;
    int tempoCalculado = ceil((falta / GRAMAS_POR_SEGUNDO) * 1000.0);

    if (tempoCalculado < TEMPO_MINIMO_ABERTURA) {
      Serial.println("Dose restante menor que a dose minima estimada; comporta mantida fechada.");
      break;
    }

    int tempo = constrain(tempoCalculado, TEMPO_MINIMO_ABERTURA, TEMPO_MAXIMO_ABERTURA);
    abrirComporta(tempo);

    float novoPeso = lerPesoEstavel(TEMPO_MAX_ESTABILIZACAO);
    if (!isfinite(novoPeso)) {
      servo.write(SERVO_FECHADO);
      return NAN;
    }

    pesoAtual = novoPeso;
    pulsos++;

    if (pesoAtual <= anterior + 0.1) semProgresso++;
    else semProgresso = 0;

    if (semProgresso >= 3) {
      Serial.println("Sem aumento de peso.");
      break;
    }
  }

  servo.write(SERVO_FECHADO);
  return pesoAtual;
}

// 20. ALIMENTACAO DO GATO
void alimentarGato(int indice) {
  Gato &gato = gatos[indice];
  int periodo = periodoAtualNumero;
  String chavePeriodoSessao = periodoAtual;
  float consumidoNoInicio = gato.consumidoPeriodo;
  float limite = obterLimitePeriodo(indice, periodo);
  float restante = limite - gato.consumidoPeriodo;

  if (restante <= 0) {
    if (intervaloPassou(ultimaNegacao[indice], INTERVALO_REGISTRO_NEGACAO)) {
      Serial.print("Limite do periodo atingido: ");
      Serial.println(gato.nome);
      registrarRelatorio("Alimentacao negada para " + String(gato.nome) +
                         ": limite do periodo atingido (" + String(limite, 2) + " g).");
    }
    return;
  }

  Serial.println("\n------------------------------");
  Serial.print("Gato: ");
  Serial.println(gato.nome);

  float pesoInicial = lerPeso();
  if (!isfinite(pesoInicial)) return;

  float quantidadeNecessaria = restante - pesoInicial;
  if (quantidadeNecessaria < 0) quantidadeNecessaria = 0;

  Serial.print("Limite: ");
  Serial.println(limite);
  Serial.print("Consumo registrado: ");
  Serial.println(gato.consumidoPeriodo);
  Serial.print("Racao na tigela: ");
  Serial.println(pesoInicial);
  Serial.print("Saldo antes da tigela: ");
  Serial.println(restante);
  Serial.print("Racao a liberar: ");
  Serial.println(quantidadeNecessaria);

  if (quantidadeNecessaria > 0.5) {
    float pesoFinal = liberarRacao(pesoInicial, quantidadeNecessaria);
    if (!isfinite(pesoFinal)) {
      Serial.println("Falha na liberacao.");
      return;
    }
  }

  float pesoAntesConsumo = lerPesoEstavel(TEMPO_MAX_ESTABILIZACAO);
  if (!isfinite(pesoAntesConsumo)) return;

  if (obterChavePeriodoAtual() != chavePeriodoSessao) {
    Serial.println("Periodo mudou durante a liberacao; sessao cancelada.");
    registrarRelatorio("Sessao cancelada: o periodo mudou durante a liberacao de racao para " +
                       String(gato.nome) + ".");
    verificarMudancaPeriodo();
    return;
  }

  Serial.println("Observando presenca e consumo.");

  unsigned long inicio = millis(), ultimaLeitura = millis();
  String codigoEsperado = String(gato.codigoRFID);
  codigoEsperado.toUpperCase();
  String motivoEncerramento = "Tempo maximo de observacao atingido.";
  bool periodoMudouDuranteSessao = false;

  struct tm horarioInicio;
  char inicioTexto[25] = "indisponivel";
  if (obterHorario(horarioInicio))
    strftime(inicioTexto, sizeof(inicioTexto), "%d/%m/%Y %H:%M:%S", &horarioInicio);

  while (millis() - inicio < TEMPO_CONSUMO) {
    if (obterChavePeriodoAtual() != chavePeriodoSessao) {
      motivoEncerramento = "Periodo mudou durante a sessao.";
      periodoMudouDuranteSessao = true;
      break;
    }

    manterWiFi();
    if (servidorIniciado) servidor.handleClient();

    String codigoLido = lerRFID();
    if (codigoLido != "") {
      if (codigoLido == codigoEsperado) ultimaLeitura = millis();
      else {
        motivoEncerramento = "Outra tag RFID foi identificada.";
        Serial.println(motivoEncerramento);
        break;
      }
    }

    if (millis() - ultimaLeitura >= TEMPO_AUSENCIA_RFID) {
      motivoEncerramento = "Tag do gato deixou de ser reconhecida.";
      Serial.println(motivoEncerramento);
      break;
    }
    delay(100);
  }

  Serial.println(motivoEncerramento);

  if (periodoMudouDuranteSessao || obterChavePeriodoAtual() != chavePeriodoSessao) {
    Serial.println("Consumo nao registrado: a sessao atravessou a troca de periodo.");
    registrarRelatorio("Sessao de " + String(gato.nome) +
                       " encerrada sem debito: houve troca de periodo. Saldo anterior preservado.");
    verificarMudancaPeriodo();
    return;
  }

  float pesoDepoisConsumo = lerPeso();
  if (!isfinite(pesoDepoisConsumo)) return;

  struct tm horarioFim;
  char fimTexto[25] = "indisponivel";
  if (obterHorario(horarioFim))
    strftime(fimTexto, sizeof(fimTexto), "%d/%m/%Y %H:%M:%S", &horarioFim);

  float consumo = pesoAntesConsumo - pesoDepoisConsumo;
  if (consumo < 0) consumo = 0;
  if (consumo > restante) consumo = restante;

  gato.consumidoPeriodo += consumo;
  if (gato.consumidoPeriodo > limite) gato.consumidoPeriodo = limite;
  salvarConsumo(indice);

  float saldo = limite - gato.consumidoPeriodo;
  if (saldo < 0) saldo = 0;

  String relatorio = "";
  relatorio += "\n========== SESSAO ==========\n";
  relatorio += "Gato: " + String(gato.nome) + "\n";
  relatorio += "RFID: " + codigoEsperado + "\n";
  relatorio += "Inicio da sessao: " + String(inicioTexto) + "\n";
  relatorio += "Fim da sessao: " + String(fimTexto) + "\n";
  relatorio += "Chave do periodo: " + chavePeriodoSessao + "\n";
  relatorio += "Consumo no inicio da sessao: " + String(consumidoNoInicio, 2) + " g\n";
  relatorio += "Limite do periodo: " + String(limite, 2) + " g\n";
  relatorio += "Racao antes da liberacao: " + String(pesoInicial, 2) + " g\n";
  relatorio += "Racao solicitada: " + String(quantidadeNecessaria, 2) + " g\n";
  relatorio += "Peso antes do consumo: " + String(pesoAntesConsumo, 2) + " g\n";
  relatorio += "Peso depois do consumo: " + String(pesoDepoisConsumo, 2) + " g\n";
  relatorio += "Consumo estimado na sessao: " + String(consumo, 2) + " g\n";
  relatorio += "Motivo do encerramento: " + motivoEncerramento + "\n";
  relatorio += "Consumo no periodo: " + String(gato.consumidoPeriodo, 2) + " g\n";
  relatorio += "Saldo restante: " + String(saldo, 2) + " g\n";
  relatorio += "============================\n";

  registrarRelatorio(relatorio);

  Serial.println("Sessao finalizada.");
  Serial.print("Consumo estimado: ");
  Serial.print(consumo);
  Serial.println(" g");
  Serial.print("Saldo restante: ");
  Serial.print(saldo);
  Serial.println(" g");
}

// 21. SETUP
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("ESTACAO AUTOMATIZADA DE ALIMENTACAO");

  memoria.begin("comedouro", false);
  RFID.begin(9600, SERIAL_8N1, RFID_RX, -1);
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(SERVO_FECHADO);

  balanca.begin(HX711_DT, HX711_SCK);
  balanca.set_scale(FATOR_CALIBRACAO);

  if (balanca.wait_ready_timeout(3000)) {
    Serial.println("HX711 pronto.");

    if (TARAR_NA_INICIALIZACAO || !memoria.isKey("offset")) {
      Serial.println("Realizando tara (a tigela deve estar vazia)...");
      balanca.tare(20);
      memoria.putLong("offset", balanca.get_offset());
      Serial.println("Tara realizada e salva.");
    } else {
      balanca.set_offset(memoria.getLong("offset", 0));
      Serial.println("Tara anterior carregada da memoria.");
    }
  } else Serial.println("Erro: HX711 nao respondeu.");

  // SISTEMA DE ARQUIVOS
  sistemaArquivosPronto = LittleFS.begin(true);

  if (sistemaArquivosPronto) {
    Serial.println("Memoria de relatorio pronta.");

    if (!LittleFS.exists("/relatorio.txt")) {
      File arquivo = LittleFS.open("/relatorio.txt", "w");
      if (arquivo) {
        arquivo.println("RELATORIO DA ESTACAO DE ALIMENTACAO");
        arquivo.close();
      }
    }
  } else Serial.println("Erro ao iniciar LittleFS.");

  // MEMORIA DE CONSUMO
  carregarConsumos();
  periodoAtual = memoria.getString("periodo", "");

  // WIFI E NTP
  conectarWiFi();

  if (horarioSincronizado) verificarMudancaPeriodo();
  else {
    Serial.println("Horario indisponivel.");
    Serial.println("Alimentacao bloqueada ate sincronizar.");
  }

  Serial.println("Sistema pronto.");
}

void loop() {
  manterWiFi();
  if (servidorIniciado) servidor.handleClient();
  if (horarioSincronizado) verificarMudancaPeriodo();

  String codigo = lerRFID();
  if (codigo == "") {
    delay(50);
    return;
  }

  if (!horarioSincronizado) {
    if (intervaloPassou(ultimoAvisoBloqueio, INTERVALO_AVISO_SERIAL))
      Serial.println("Horario nao sincronizado.");
    return;
  }

  int indice = identificarGato(codigo);
  if (indice < 0) {
    if (intervaloPassou(ultimoAvisoTagDesconhecida, INTERVALO_AVISO_SERIAL)) {
      Serial.print("Gato nao cadastrado. Codigo lido: ");
      Serial.println(codigo);
      registrarRelatorio("Tag nao cadastrada lida: " + codigo);
    }
    return;
  }

  if (periodoAtualNumero < 0) {
    if (intervaloPassou(ultimoAvisoBloqueio, INTERVALO_AVISO_SERIAL))
      Serial.println("Periodo ainda nao identificado.");
    return;
  }

  if (!BALANCA_CALIBRADA) {
    if (intervaloPassou(ultimoAvisoBloqueio, INTERVALO_AVISO_SERIAL)) {
      Serial.println("Balanca nao calibrada.");
      Serial.println("Liberacao bloqueada.");
    }
    return;
  }

  if (GRAMAS_POR_SEGUNDO <= 0) {
    if (intervaloPassou(ultimoAvisoBloqueio, INTERVALO_AVISO_SERIAL)) {
      Serial.println("Servo ainda nao calibrado.");
      Serial.println("Liberacao bloqueada.");
    }
    return;
  }

  alimentarGato(indice);
  Serial.println("Aguardando proxima identificacao.");
}
