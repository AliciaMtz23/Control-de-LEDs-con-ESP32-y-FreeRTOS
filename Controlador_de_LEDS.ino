#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// ========== CONFIGURACIÓN ==========
const char* ssid = "RED WIFI";
const char* password = "CONTRASEÑA";
WiFiServer servidor(80);

const int NUM_LEDS = 5;
const int pinesLed[] = {2, 4, 5, 18, 19};
const char* nombresLed[] = {"Blanco", "Verde", "Azul", "Rojo", "Amarillo"};

// ========== VARIABLES COMPARTIDAS (PROTEGIDAS) ==========
bool estadosLed[NUM_LEDS] = {0};
bool modoRgb = false;
int pasoRgb = 0;

// ========== OBJETOS FREERTOS ==========
SemaphoreHandle_t mutexLed;           // Protege estadosLed[]
SemaphoreHandle_t mutexRgb;           // Protege modoRgb y pasoRgb
QueueHandle_t colaComandos;           // Cola de comandos HTTP

// Estructura para comandos
typedef struct {
  enum { CMD_TOGGLE, CMD_TODOS_ON, CMD_TODOS_OFF, CMD_RGB_TOGGLE } tipo;
  int indiceLed;
} Comando;

// ========== TAREA 1: SERVIDOR WEB (Prioridad ALTA - 3) ==========
void tareaServidorWeb(void *parametro) {
  Serial.println("[TAREA] Servidor Web iniciado - Prioridad ALTA");
  
  while(1) {
    WiFiClient cliente = servidor.available();
    if (cliente) {
      Serial.println("[WEB] Cliente conectado");
      
      unsigned long timeout = millis() + 3000;
      while (!cliente.available() && millis() < timeout) {
        vTaskDelay(1 / portTICK_PERIOD_MS); // Yield cooperativo
      }
      
      if (millis() < timeout) {
        String peticion = cliente.readStringUntil('\r');
        cliente.flush();
        
        // Procesar petición y enviar a cola
        Comando cmd;
        bool enviarCmd = false;
        
        if (peticion.indexOf("/toggleRGB") != -1) {
          cmd.tipo = Comando::CMD_RGB_TOGGLE;
          enviarCmd = true;
        } else if (peticion.indexOf("/allOn") != -1) {
          cmd.tipo = Comando::CMD_TODOS_ON;
          enviarCmd = true;
        } else if (peticion.indexOf("/allOff") != -1) {
          cmd.tipo = Comando::CMD_TODOS_OFF;
          enviarCmd = true;
        } else {
          for (int i = 0; i < NUM_LEDS; i++) {
            if (peticion.indexOf("/toggle" + String(i+1)) != -1) {
              cmd.tipo = Comando::CMD_TOGGLE;
              cmd.indiceLed = i;
              enviarCmd = true;
              break;
            }
          }
        }
        
        if (enviarCmd) {
          xQueueSend(colaComandos, &cmd, portMAX_DELAY);
          Serial.println("[WEB] Comando enviado a cola");
        }
        
        // Enviar respuesta
        if (peticion.indexOf("/status") != -1) {
          enviarJSON(cliente);
        } else {
          enviarHTML(cliente);
        }
      }
      cliente.stop();
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // Liberar CPU
  }
}

// ========== TAREA 2: PROCESADOR DE COMANDOS (Prioridad MEDIA - 2) ==========
void tareaProcesadorComandos(void *parametro) {
  Serial.println("[TAREA] Procesador de Comandos iniciado - Prioridad MEDIA");
  Comando cmd;
  
  while(1) {
    if (xQueueReceive(colaComandos, &cmd, portMAX_DELAY)) {
      Serial.println("[CMD] Procesando comando");
      
      switch(cmd.tipo) {
        case Comando::CMD_TOGGLE:
          if (xSemaphoreTake(mutexLed, portMAX_DELAY)) {
            if (!modoRgb) {
              estadosLed[cmd.indiceLed] = !estadosLed[cmd.indiceLed];
              digitalWrite(pinesLed[cmd.indiceLed], estadosLed[cmd.indiceLed]);
              Serial.printf("[CMD] LED %d: %s\n", cmd.indiceLed+1, 
                           estadosLed[cmd.indiceLed] ? "ON" : "OFF");
            }
            xSemaphoreGive(mutexLed);
          }
          break;
          
        case Comando::CMD_TODOS_ON:
          if (xSemaphoreTake(mutexRgb, portMAX_DELAY)) {
            modoRgb = false;
            xSemaphoreGive(mutexRgb);
          }
          if (xSemaphoreTake(mutexLed, portMAX_DELAY)) {
            for (int i = 0; i < NUM_LEDS; i++) {
              estadosLed[i] = true;
              digitalWrite(pinesLed[i], HIGH);
            }
            Serial.println("[CMD] Todos los LEDs ON");
            xSemaphoreGive(mutexLed);
          }
          break;
          
        case Comando::CMD_TODOS_OFF:
          if (xSemaphoreTake(mutexRgb, portMAX_DELAY)) {
            modoRgb = false;
            xSemaphoreGive(mutexRgb);
          }
          if (xSemaphoreTake(mutexLed, portMAX_DELAY)) {
            for (int i = 0; i < NUM_LEDS; i++) {
              estadosLed[i] = false;
              digitalWrite(pinesLed[i], LOW);
            }
            Serial.println("[CMD] Todos los LEDs OFF");
            xSemaphoreGive(mutexLed);
          }
          break;
          
        case Comando::CMD_RGB_TOGGLE:
          if (xSemaphoreTake(mutexRgb, portMAX_DELAY)) {
            modoRgb = !modoRgb;
            Serial.printf("[CMD] Modo RGB: %s\n", modoRgb ? "ON" : "OFF");
            if (!modoRgb) {
              pasoRgb = 0;
              if (xSemaphoreTake(mutexLed, portMAX_DELAY)) {
                for (int i = 0; i < NUM_LEDS; i++) {
                  estadosLed[i] = false;
                  digitalWrite(pinesLed[i], LOW);
                }
                xSemaphoreGive(mutexLed);
              }
            }
            xSemaphoreGive(mutexRgb);
          }
          break;
      }
    }
  }
}

// ========== TAREA 3: EFECTO RGB (Prioridad BAJA - 1) ==========
void tareaEfectoRGB(void *parametro) {
  Serial.println("[TAREA] Efecto RGB iniciado - Prioridad BAJA");
  
  // Patrones RGB: LED 1=Blanco, LED 2=Verde, LED 3=Azul, LED 4=Rojo, LED 5=Amarillo
  // Solo encendemos: Verde (índice 1), Azul (índice 2), Rojo (índice 3)
  int patrones[][5] = {
    {0,0,0,1,0}, // Solo Rojo
    {0,1,0,0,0}, // Solo Verde  
    {0,0,1,0,0}, // Solo Azul
    {0,0,0,1,0}, // Solo Rojo
    {0,1,0,0,0}, // Solo Verde
    {0,0,1,0,0}, // Solo Azul
    {0,0,0,1,0}, // Solo Rojo
    {0,1,0,0,0}, // Solo Verde
    {0,0,1,0,0}, // Solo Azul
    {0,0,0,0,0}  // Todos apagados
  };
  
  while(1) {
    bool rgbActivo = false;
    
    // Verificar si RGB está activo
    if (xSemaphoreTake(mutexRgb, 10 / portTICK_PERIOD_MS)) {
      rgbActivo = modoRgb;
      xSemaphoreGive(mutexRgb);
    }
    
    if (rgbActivo) {
      // Aplicar patrón RGB
      if (xSemaphoreTake(mutexLed, 10 / portTICK_PERIOD_MS)) {
        for (int i = 0; i < NUM_LEDS; i++) {
          estadosLed[i] = patrones[pasoRgb][i];
          digitalWrite(pinesLed[i], estadosLed[i]);
        }
        xSemaphoreGive(mutexLed);
      }
      
      // Avanzar al siguiente paso
      if (xSemaphoreTake(mutexRgb, 10 / portTICK_PERIOD_MS)) {
        pasoRgb = (pasoRgb + 1) % 10;
        xSemaphoreGive(mutexRgb);
      }
      
      vTaskDelay(300 / portTICK_PERIOD_MS); // 300ms entre cambios
    } else {
      vTaskDelay(100 / portTICK_PERIOD_MS); // Espera más cuando está inactivo
    }
  }
}

// ========== TAREA 4: MONITOR DEL SISTEMA (Prioridad BAJA - 1) ==========
void tareaMonitorSistema(void *parametro) {
  Serial.println("[TAREA] Monitor del Sistema iniciado - Prioridad BAJA");
  
  while(1) {
    Serial.println("\n========== ESTADO DEL SISTEMA ==========");
    Serial.printf("Heap libre: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Tareas activas: %d\n", uxTaskGetNumberOfTasks());
    
    if (xSemaphoreTake(mutexLed, 100 / portTICK_PERIOD_MS)) {
      Serial.print("LEDs: ");
      for (int i = 0; i < NUM_LEDS; i++) {
        Serial.printf("%d:%s ", i+1, estadosLed[i] ? "ON" : "OFF");
      }
      Serial.println();
      xSemaphoreGive(mutexLed);
    }
    
    if (xSemaphoreTake(mutexRgb, 100 / portTICK_PERIOD_MS)) {
      Serial.printf("Modo RGB: %s\n", modoRgb ? "ACTIVO" : "INACTIVO");
      xSemaphoreGive(mutexRgb);
    }
    
    Serial.println("========================================\n");
    vTaskDelay(5000 / portTICK_PERIOD_MS); // Reporte cada 5 segundos
  }
}

// ========== FUNCIONES AUXILIARES ==========
void enviarJSON(WiFiClient &c) {
  c.println("HTTP/1.1 200 OK\r\nContent-type:application/json\r\nConnection:close\r\n");
  c.print("{\"rgbMode\":");
  
  if (xSemaphoreTake(mutexRgb, 100 / portTICK_PERIOD_MS)) {
    c.print(modoRgb ? "true" : "false");
    xSemaphoreGive(mutexRgb);
  }
  
  if (xSemaphoreTake(mutexLed, 100 / portTICK_PERIOD_MS)) {
    for (int i = 0; i < NUM_LEDS; i++) {
      c.print(",\"led");
      c.print(i+1);
      c.print("\":");
      c.print(estadosLed[i] ? "true" : "false");
    }
    xSemaphoreGive(mutexLed);
  }
  c.println("}");
}

void enviarHTML(WiFiClient &c) {
  c.println("HTTP/1.1 200 OK\r\nContent-type:text/html\r\nConnection:close\r\n");
  c.println("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP32 FreeRTOS</title><style>");
  c.println("*{margin:0;padding:0;box-sizing:border-box}body{display:flex;justify-content:center;align-items:center;min-height:100vh;background:#0a0a0a;font-family:-apple-system,sans-serif;padding:20px}");
  c.println(".c{background:#fff;padding:40px;border-radius:24px;box-shadow:0 20px 60px rgba(0,0,0,.15);max-width:420px;width:100%;border:1px solid #f0f0f0}h1{font-size:28px;font-weight:300;color:#000;margin-bottom:10px;text-align:center;letter-spacing:2px}");
  c.println(".badge{background:#000;color:#fff;padding:6px 12px;border-radius:6px;font-size:10px;display:inline-block;margin-bottom:25px;letter-spacing:1px;font-weight:500}");
  c.println(".rgb{background:#000;color:#fff;width:100%;margin-bottom:20px;padding:20px;border:none;border-radius:12px;font-size:14px;font-weight:500;cursor:pointer;transition:.3s;letter-spacing:1px}.rgb:hover{background:#1a1a1a}");
  c.println(".rgb.on{background:#fff;color:#000;border:2px solid #000;animation:pulse 2s infinite}@keyframes pulse{0%,100%{box-shadow:0 0 0 0 rgba(0,0,0,.4)}50%{box-shadow:0 0 0 8px rgba(0,0,0,0)}}");
  c.println(".g{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:25px}.b{padding:18px;border:2px solid #000;background:#fff;border-radius:12px;font-size:13px;font-weight:500;cursor:pointer;transition:.3s;letter-spacing:.5px;color:#000}.b:hover{background:#000;color:#fff}");
  c.println(".led{padding:18px;margin-bottom:12px;background:#fafafa;border:1px solid #e5e5e5;border-radius:12px;display:flex;justify-content:space-between;align-items:center;transition:.2s}.led:hover{background:#f5f5f5}");
  c.println(".sw{position:relative;display:inline-block;width:52px;height:28px}.sw input{opacity:0;width:0;height:0}.sl{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#e0e0e0;transition:.3s;border-radius:28px;border:1px solid #d0d0d0}.sl:before{position:absolute;content:'';height:22px;width:22px;left:2px;bottom:2px;background:#fff;transition:.3s;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,.1)}");
  c.println("input:checked+.sl{background:#000;border-color:#000}input:checked+.sl:before{transform:translateX(24px)}.n{font-size:14px;font-weight:500;margin-bottom:4px;color:#000;letter-spacing:.3px}.s{font-size:11px;color:#999}</style>");
  c.println("<script>function t(n){fetch('/toggle'+n).then(()=>setTimeout(u,300))}function on(){fetch('/allOn').then(()=>setTimeout(u,300))}function off(){fetch('/allOff').then(()=>setTimeout(u,300))}function rgb(){fetch('/toggleRGB').then(()=>setTimeout(u,300))}");
  c.print("function u(){fetch('/status').then(r=>r.json()).then(d=>{for(let i=1;i<=");
  c.print(NUM_LEDS);
  c.println(";i++){document.getElementById('c'+i).checked=d['led'+i];document.getElementById('t'+i).textContent=d['led'+i]?'Encendido':'Apagado'}const b=document.getElementById('rb');d.rgbMode?(b.classList.add('on'),b.textContent='🌈 RGB ACTIVO'):(b.classList.remove('on'),b.textContent='🌈 MODO RGB')})}setInterval(u,500)</script>");
  c.println("</head><body><div class='c'><h1>ESP32 CONTROL</h1><div style='text-align:center'>");
  c.println("<span class='badge'>FREERTOS MULTITAREA</span></div>");
  
  bool modoRgbActual = false;
  if (xSemaphoreTake(mutexRgb, 100 / portTICK_PERIOD_MS)) {
    modoRgbActual = modoRgb;
    xSemaphoreGive(mutexRgb);
  }
  
  c.print("<button id='rb' class='rgb");
  if (modoRgbActual) c.print(" on");
  c.print("' onclick='rgb()'>");
  c.print(modoRgbActual ? "🌈 RGB ACTIVO" : "🌈 MODO RGB");
  c.println("</button><div class='g'><button class='b' onclick='on()'>Encender</button><button class='b' onclick='off()'>Apagar</button></div>");
  
  if (xSemaphoreTake(mutexLed, 100 / portTICK_PERIOD_MS)) {
    for (int i = 0; i < NUM_LEDS; i++) {
      c.print("<div class='led'><div><div class='n'>LED ");
      c.print(i+1);
      c.print(" (");
      c.print(nombresLed[i]);
      c.print(")</div><div class='s' id='t");
      c.print(i+1);
      c.print("'>");
      c.print(estadosLed[i] ? "Encendido" : "Apagado");
      c.print("</div></div><label class='sw'><input type='checkbox' id='c");
      c.print(i+1);
      c.print("' onchange='t(");
      c.print(i+1);
      c.print(")'");
      if (estadosLed[i]) c.print(" checked");
      c.println("><span class='sl'></span></label></div>");
    }
    xSemaphoreGive(mutexLed);
  }
  
  c.print("<div style='margin-top:20px;padding-top:15px;border-top:2px solid #e0e0e0;text-align:center;font-size:11px;color:#999'>ESP32 FreeRTOS • ");
  c.print(WiFi.localIP());
  c.println("</div></div></body></html>");
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== SISTEMA CON FREERTOS ===");
  
  // Inicializar pines
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(pinesLed[i], OUTPUT);
    digitalWrite(pinesLed[i], LOW);
  }
  
  // Crear semáforos
  mutexLed = xSemaphoreCreateMutex();
  mutexRgb = xSemaphoreCreateMutex();
  
  // Crear cola de comandos
  colaComandos = xQueueCreate(10, sizeof(Comando));
  
  // Conectar WiFi
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED && millis() < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi conectado!");
    Serial.print("IP: http://");
    Serial.println(WiFi.localIP());
  }
  
  servidor.begin();
  
  // Crear tareas FreeRTOS con diferentes prioridades
  xTaskCreatePinnedToCore(tareaServidorWeb, "ServidorWeb", 8192, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(tareaProcesadorComandos, "Comandos", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(tareaEfectoRGB, "RGB", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(tareaMonitorSistema, "Monitor", 2048, NULL, 1, NULL, 0);
  
  Serial.println("✓ Tareas FreeRTOS iniciadas");
  Serial.println("  - ServidorWeb (Core 1, Prioridad 3)");
  Serial.println("  - Comandos (Core 1, Prioridad 2)");
  Serial.println("  - RGB Effect (Core 0, Prioridad 1)");
  Serial.println("  - Monitor (Core 0, Prioridad 1)");
  Serial.println("===============================\n");
}

// ========== LOOP (VACÍO - TODO LO MANEJA FREERTOS) ==========
void loop() {
  // El loop() está vacío porque FreeRTOS maneja todo
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
