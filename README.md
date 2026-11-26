# Desenvolvimento e análise de um sistema automatizado de alimentação para múltiplos gatos

Firmware do protótipo desenvolvido no Trabalho de Conclusão de Curso do curso de Engenharia da Computação do Centro Universitário Maurício de Nassau (UNINASSAU), 2026.

- **Autor:** Cláudio Cordeiro Gonçalves de Oliveira
- **Orientador:** Prof. Bernardo Caldas

## Visão geral

O sistema identifica cada gato por uma etiqueta RFID presa à coleira, calcula quanto falta para completar a porção programada do período, libera a ração de um reservatório por meio de um servomotor e acompanha o consumo com uma célula de carga. Cada sessão de alimentação é registrada em um arquivo de texto que pode ser baixado pela rede local.

O protótipo usa uma **tigela compartilhada** e **não impede fisicamente** o acesso de outro animal. O registro é feito por sessão, isto é, enquanto o gato identificado permanece associado à tigela.

## Como funciona

1. O leitor RDM6300 lê a etiqueta do gato (UART, 9600 baud) e o código é comparado com os cadastrados no firmware. Tags desconhecidas são ignoradas.
2. O horário é obtido por NTP (UTC−3). Sem horário sincronizado, a alimentação fica bloqueada.
3. O dia é dividido em três períodos: manhã (06h–12h), tarde (12h–18h) e noite (18h–06h). Cada gato tem um limite de ração por período.
4. A quantidade a liberar é o saldo do período menos a ração que já está na tigela.
5. O servomotor abre a comporta por um tempo calculado com a taxa de liberação (g/s). Depois de cada acionamento, a balança é lida até estabilizar, e novos acionamentos são feitos se necessário (com limite de pulsos).
6. A sessão de consumo dura no máximo 60 s e termina se a tag deixar de ser lida por mais de 5 s ou se outra tag for identificada.
7. A diferença de massa na tigela estima o consumo, que é somado ao do período e salvo na memória não volátil.
8. Cada sessão é gravada em `relatorio.txt` (LittleFS), disponível em `http://<IP do ESP32>/relatorio.txt`.

## Hardware

| Componente | Função |
|---|---|
| ESP32-S3 | Unidade central, Wi-Fi para obter a hora (NTP) |
| Leitor RFID RDM6300 (125 kHz) + etiquetas EM4100 | Identificação individual dos gatos |
| Célula de carga tipo barra de 1 kg + HX711 | Medição da massa de ração na tigela |
| Microservo MG90S | Acionamento da comporta do reservatório |
| Fonte de 5 V, protoboard e jumpers | Alimentação e ligações |

### Ligações

**Servomotor MG90S**

| Fio | Conexão |
|---|---|
| Marrom (GND) | GND da fonte externa |
| Vermelho (VCC) | 5 V da fonte externa |
| Laranja (sinal) | GPIO 10 |

**RDM6300**

| Pino | Conexão |
|---|---|
| VCC | 5 V da fonte externa |
| GND | GND da fonte externa |
| TX | GPIO 16 |
| RX | Desconectado |

**Célula de carga → HX711**

| Fio da célula | Pino do HX711 |
|---|---|
| Vermelho | E+ |
| Preto | E− |
| Verde | A+ |
| Branco | A− |

**HX711 → ESP32-S3**

| Pino | Conexão |
|---|---|
| VCC | 3,3 V |
| GND | GND |
| DT | GPIO 4 |
| SCK | GPIO 5 |

Os GND do ESP32-S3, da fonte externa, do RDM6300 e do HX711 devem ser ligados entre si. O ESP32-S3 é alimentado pela USB.

## Software

- Arduino IDE 2.3.11
- Pacote Arduino-ESP32 (Espressif) versão 3.3.7, placa ESP32-S3
- Bibliotecas instaladas pelo Gerenciador de Bibliotecas: **ESP32Servo** e **HX711**
- Bibliotecas nativas do pacote ESP32: WiFi, WebServer, LittleFS, Preferences, time

Monitor Serial em **115200 baud**.

## Estrutura do repositório

```
firmware/
  estacao_alimentacao/
    estacao_alimentacao.ino
README.md
```

O Arduino IDE exige que a pasta do sketch tenha o mesmo nome do arquivo `.ino`.

## Configuração

Antes de gravar, ajuste no início do `.ino`:

1. **Wi-Fi:** `WIFI_SSID` e `WIFI_PASSWORD`. Não publique a senha real no repositório.
2. **Códigos RFID:** no vetor `gatos[]`, o campo `codigoRFID` de cada gato (10 caracteres hexadecimais). Com o Monitor Serial aberto, aproxime a etiqueta do leitor. Uma tag não cadastrada gera a mensagem "Gato nao cadastrado. Codigo lido: ...".
3. **Limites por período:** o campo `limites[3]` de cada gato (manhã, tarde, noite), em gramas. Os valores do protótipo vieram da tabela orientativa da ração PremieR Gatos Castrados:

| | Gato 1 (fêmea, 4,2 kg) | Gato 2 (macho, 5 kg) |
|---|---|---|
| Manhã | 14 g | 17 g |
| Tarde | 14 g | 17 g |
| Noite | 15 g | 18 g |
| Total diário | 43 g | 52 g |

## Calibração

**Balança**

1. Com a tigela vazia, o firmware faz a tara na primeira inicialização e salva o offset. Para repetir a tara, defina `TARAR_NA_INICIALIZACAO = true`, grave, e volte para `false`.
2. Coloque uma massa conhecida (o protótipo usou 1 kg) e calcule o fator: `F = (R_ref − R_zero) / M_ref`, com as leituras brutas e a massa em gramas. No protótipo, F = 176,222.
3. Atualize `FATOR_CALIBRACAO`.
4. Defina `BALANCA_CALIBRADA = true`. **Com `false`, o sistema bloqueia a liberação de ração.**

**Liberação**

Mantenha a comporta aberta até a tigela receber uma massa conhecida, cronometre e calcule `GRAMAS_POR_SEGUNDO = massa (g) / tempo (s)`. No protótipo, 350 g em cerca de 100 s resultaram em 3,5 g/s. Cada pulso do servo é limitado a 2 s e a balança é lida entre os pulsos.

## Parâmetros principais

| Parâmetro | Valor no protótipo |
|---|---|
| `SERVO_FECHADO` / `SERVO_ABERTO` | 0° / 30° |
| `GRAMAS_POR_SEGUNDO` | 3,5 |
| `FATOR_CALIBRACAO` | 176,222 |
| `TEMPO_MAXIMO_ABERTURA` | 2000 ms por pulso |
| `MAX_PULSOS` | 20 |
| `TOLERANCIA_ESTABILIDADE` | 0,3 g |
| `TEMPO_CONSUMO` | 60 s |
| `TEMPO_AUSENCIA_RFID` | 5 s |

## Relatório

O arquivo `relatorio.txt` guarda, por sessão: gato, código RFID, início e fim, período, limite, massas antes e depois, consumo estimado, motivo do encerramento e saldo restante. Com o ESP32-S3 conectado ao Wi-Fi, acesse `http://<IP>/` (o IP aparece no Monitor Serial) e baixe o arquivo.

## Limitações conhecidas

- A célula de carga apresentou oscilação nas leituras, e a taxa de liberação foi obtida em uma única medição. A dosagem e o consumo devem ser vistos como estimativas.
- O ruído do motor durante a liberação assustava o gato, que se afastava da tigela.
- A tigela é compartilhada e não há bloqueio de acesso por animal.
- O RDM6300 e as etiquetas EM4100 (125 kHz) não seguem as normas ISO 11784/11785 e não leem o microchip implantado.
- Os testes foram funcionais e exploratórios, com poucas repetições e sem tratamento estatístico.

## Como citar

OLIVEIRA, Cláudio Cordeiro Gonçalves de. *Desenvolvimento e análise de um sistema automatizado de alimentação para múltiplos gatos: firmware*. GitHub, 2026. Disponível em: https://github.com/[usuario]/[repositorio]. Acesso em: [data].

## Licença

[Escolha uma licença, por exemplo MIT, ou remova esta seção.]
