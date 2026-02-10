/*
 * =======================================================================================
 * SQUAWK BOX v9.0 - MASTER LOGGING EDITION
 * =======================================================================================
 * AUTHOR: Jason Edgar (Orillia, ON)
 * HARDWARE: Particle Photon 2 | LCD 16x2 (I2C) | Active Buzzer | Momentary Switch
 * * CORE FEATURES:
 * 1. MARKET ENGINE: Calculates Momentum (EMA Spread) & Velocity Acceleration.
 * 2. WEB DASHBOARD: Live 600px Responsive UI with Auto-Updating Alerts & Graph.
 * 3. SMART POLLING: Aggressive 2s updates during Market Open/Close; 60s during Sleep.
 * 4. EVENT LOGGING: Circular buffer tracks last 5 Bull/Bear events with timestamps.
 * 5. AUDIO: Distinct tones for "Breakout" (Solid) vs "Acceleration" (Stutter).
 * =======================================================================================
 */

#include "Particle.h"
#include <LiquidCrystal_I2C_Spark.h> 
#include <cmath> 

// Enable Threading so WiFi doesn't block the Audio/LCD loop
SYSTEM_THREAD(ENABLED);

// --- HARDWARE PIN DEFINITIONS ---
LiquidCrystal_I2C lcd(0x27, 16, 2);   // I2C Address 0x27
TCPServer server(80);                 // Web Server Port
const int BUZZER_PIN = D2;            // Active Buzzer (PWM Capable)
const int BUTTON_PIN = D3;            // User Button (Pull-Up)

// --- STRATEGY PARAMETERS (TUNED FOR 2-4s POLLING) ---
// FAST = Reaction Time | SLOW = Trend Filter | CHOP = Noise Gate
const float P_SPY_FAST = 0.22; const float P_SPY_SLOW = 0.10; const float P_SPY_CHOP = 0.05;
const float P_QQQ_FAST = 0.25; const float P_QQQ_SLOW = 0.12; const float P_QQQ_CHOP = 0.06;
const float P_IWM_FAST = 0.28; const float P_IWM_SLOW = 0.14; const float P_IWM_CHOP = 0.07;

// --- CUSTOM LCD ICONS ---
byte iconSpeaker[8] = { 0b00001, 0b00011, 0b01111, 0b01111, 0b01111, 0b01111, 0b00011, 0b00001 };
byte iconMute[8]    = { 0b10001, 0b01011, 0b01101, 0b01111, 0b01111, 0b01011, 0b10011, 0b00001 };
byte iconUp[8]      = { 0b00111, 0b00011, 0b00101, 0b01001, 0b10000, 0b00000, 0b00000, 0b00000 };
byte iconDown[8]    = { 0b00000, 0b00000, 0b00000, 0b10000, 0b01001, 0b00101, 0b00011, 0b00111 };
byte iconFlat[8]    = { 0b00000, 0b00000, 0b00000, 0b11111, 0b11111, 0b00000, 0b00000, 0b00000 };

// --- PERSISTENT SETTINGS (SAVED TO EEPROM) ---
struct Config {
    uint8_t version;     // Used to force a reset if we change this struct
    float alphaFast;     
    float alphaSlow;     
    float chopLimit;     
    bool isMuted;        
    bool backlightOn;    
    char symbol[8];      // SPY, QQQ, or IWM
    uint16_t checksum;
};
Config settings;

// --- ALERT LOGGING SYSTEM ---
struct Alert {
    String time;  // HH:MM:SS
    String type;  // "BULL RUSH", "BEAR DUMP", etc.
    float val;    // The velocity value at trigger
};
Alert alertLog[5]; // Circular Buffer (Stores last 5 events)

// --- SYSTEM STATE VARIABLES ---
// Volatile used for variables shared between Network Thread and Main Loop
volatile float emaFast = 0, emaSlow = 0, diff = 0;
volatile float lastPrice = 0;
bool initialized = false;        // Waits for first valid quote
unsigned long lastRequest = 0;   // Timer for API polling
bool forceFullUpdate = false;    // Flags LCD to redraw everything
int lastButtonState = HIGH;      

// --- LCD CACHE (PREVENTS FLICKER) ---
char lastRow0_Text[17] = ""; 
char lastRow1_Text[17] = "";
byte lastIconTrend = 255; 
int lastIconMute = -1; // -1 Forces initial draw on boot

// --- GRAPH HISTORY BUFFER ---
const int HISTORY_SIZE = 120; // 120 points (4 mins @ 2s, 8 mins @ 4s)
volatile float velocityHistory[HISTORY_SIZE]; 
volatile int historyHead = 0;

// --- AUDIO STATE MACHINE ---
enum BuzzerState { BZ_IDLE, BZ_TONE_1, BZ_TONE_2, BZ_GAP };
BuzzerState bzState = BZ_IDLE;
unsigned long bzTimer = 0; unsigned long bzDuration = 0; 

// Duration Constants (ms)
const int DUR_BULLISH = 200;  // Solid Beep
const int DUR_BEARISH = 1000; // Long Tone
const int DUR_CHOP = 100;     // Short Blip
const int DUR_GAP = 100;      // Silence between stutters

// --- FUNCTION PROTOTYPES ---
void handleQuote(const char *event, const char *data);
unsigned long getSmartInterval();
void applySymbolPreset(const char* sym);
void logEvent(String type, float v);
void updateLCD();
void updateLED();
void updateBuzzer();
void handleWebTraffic();
void handleButton();
void loadSettings();
void saveSettings();

// =======================================================================================
// SYSTEM SETUP
// =======================================================================================
void setup() {
    // 1. Hardware Init
    WiFi.selectAntenna(ANT_EXTERNAL); // Critical for Orillia signal stability
    Time.zone(-5);                    // Eastern Standard Time
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);
    
    // Take control of the Photon's RGB LED
    RGB.control(true); 
    RGB.brightness(255); 

    // 2. LCD Init
    lcd.init();
    lcd.createChar(0, iconSpeaker); lcd.createChar(1, iconMute);  
    lcd.createChar(2, iconUp); lcd.createChar(3, iconDown); lcd.createChar(4, iconFlat);     
    
    // 3. Load User Preferences
    loadSettings(); 
    if(settings.backlightOn) lcd.backlight(); else lcd.noBacklight();

    // 4. Boot Screen
    lcd.setCursor(0, 0); lcd.print("SQUAWK BOX v9.0");
    lcd.setCursor(0, 1); lcd.print("LOGGING ED.");
    
    // 5. Network Wait
    waitFor(WiFi.ready, 15000);
    // DHCP Safety Loop
    while (WiFi.localIP().toString() == "0.0.0.0") { Particle.process(); delay(500); }

    // 6. Start Services
    server.begin(); 
    Particle.subscribe("hook-response/finnhub_quote", handleQuote);
    
    // 7. Ready Chirp
    digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); 
    delay(2000); lcd.clear();
    forceFullUpdate = true;
}

// =======================================================================================
// MAIN LOOP
// =======================================================================================
void loop() {
    handleButton();      // Check physical input
    handleWebTraffic();  // Check for dashboard requests
    updateBuzzer();      // Process audio queue
    updateLCD();         // Refresh screen (if needed)
    updateLED();         // Pulse heartbeat

    // SMART POLLING ENGINE
    // Dynamically adjusts poll rate based on time of day
    unsigned long dynamicDelay = getSmartInterval() * 1000UL;
    if (millis() - lastRequest > dynamicDelay) {
        lastRequest = millis();
        if (Particle.connected()) Particle.publish("finnhub_quote", settings.symbol, PRIVATE);
    }
}

// =======================================================================================
// MARKET ENGINE (EMA & ACCELERATION LOGIC)
// =======================================================================================
void handleQuote(const char *event, const char *data) {
    float p = atof(data); if (p <= 0) return; 
    lastPrice = p; 
    
    if (!initialized) { emaFast=emaSlow=lastPrice; diff=0; initialized=true; forceFullUpdate=true; return; } 

    float prevDiff = diff; 
    emaFast = (lastPrice * settings.alphaFast) + (emaFast * (1.0 - settings.alphaFast));
    emaSlow = (lastPrice * settings.alphaSlow) + (emaSlow * (1.0 - settings.alphaSlow));
    diff = emaFast - emaSlow; 
    
    velocityHistory[historyHead] = diff; 
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    
    // --- ALERT LOGIC ---
    
    // 1. Check for Breakouts (Leaving Chop)
    if (abs(diff) > settings.chopLimit) {
        
        // BULL SIDE
        if (diff > 0) {
            if (prevDiff <= settings.chopLimit) { // Just broke out
                bzDuration = DUR_BULLISH; bzTimer = millis(); bzState = BZ_TONE_1; 
                logEvent("BULL BREAK", diff);
            }
            else if (diff > (prevDiff * 1.20)) { // Accelerating
                bzDuration = 100; bzTimer = millis(); bzState = BZ_TONE_2; 
                logEvent("BULL RUSH", diff);
            }
        }
        // BEAR SIDE
        else {
            if (prevDiff >= -settings.chopLimit) { // Just broke down
                bzDuration = DUR_BEARISH; bzTimer = millis(); bzState = BZ_TONE_1; 
                logEvent("BEAR BREAK", diff);
            }
            else if (diff < (prevDiff * 1.20)) { // Accelerating down
                 bzDuration = 100; bzTimer = millis(); bzState = BZ_TONE_2; 
                 logEvent("BEAR DUMP", diff);
            }
        }
    }
    // 2. Check for "Trend Death" (Returning to Chop)
    // *** THIS IS THE NEW FIX ***
    else if (abs(prevDiff) > settings.chopLimit) {
        // We were trending, now we are not.
        bzDuration = DUR_CHOP; bzTimer = millis(); bzState = BZ_TONE_1; // Short "Blip"
        logEvent("TREND END", diff);
    }
}

// Helper: Adds event to Circular Log
void logEvent(String type, float v) {
    // Shift old logs down
    for(int i=4; i>0; i--) { alertLog[i] = alertLog[i-1]; }
    // Add new log at top
    alertLog[0].time = Time.format(Time.now(), "%T"); 
    alertLog[0].type = type;
    alertLog[0].val = v;
}

// =======================================================================================
// WEB DASHBOARD ENGINE (RESPONSIVE & AUTO-UPDATING)
// =======================================================================================
void handleWebTraffic() {
    TCPClient client = server.available();
    if (!client) return;

    // Safety Timeout
    unsigned long timeout = millis();
    while(!client.available() && (millis() - timeout < 600)) { Particle.process(); }
    if (!client.available()) { client.stop(); return; }

    String req = client.readStringUntil('\n');
    while(client.available()) client.read(); 

    // --- JSON API ENDPOINT (Called by Browser JS) ---
    if (req.indexOf("GET /data") != -1) {
        client.println("HTTP/1.1 200 OK\nContent-Type: application/json\nConnection: close\n");
        client.print("{\"price\":"); client.print(lastPrice, 2);
        client.print(",\"diff\":"); client.print(diff, 3);
        
        // Inject Recent Alerts Array
        client.print(",\"alerts\":[");
        for(int i=0; i<5; i++) {
            if (alertLog[i].type != "") {
                client.print("{\"t\":\"" + alertLog[i].time + "\",");
                client.print("\"e\":\"" + alertLog[i].type + "\",");
                client.print("\"v\":" + String(alertLog[i].val, 2) + "}");
                if (i < 4 && alertLog[i+1].type != "") client.print(",");
            }
        }
        client.print("],");

        // Inject Graph History Array
        client.print("\"history\":[");
        for(int i=0; i<HISTORY_SIZE; i++) {
            float v = velocityHistory[(historyHead + i) % HISTORY_SIZE];
            if(std::isnan(v)) client.print("0.0"); else client.print(v, 2);
            if(i < HISTORY_SIZE - 1) client.print(",");
        }
        client.print("]}"); client.stop(); return;
    }

    // --- COMMAND PROCESSING ---
    bool saveNeeded = false;
    if (req.indexOf("sym=SPY") != -1) { strcpy(settings.symbol, "SPY"); applySymbolPreset("SPY"); initialized = false; saveNeeded = true; }
    if (req.indexOf("sym=QQQ") != -1) { strcpy(settings.symbol, "QQQ"); applySymbolPreset("QQQ"); initialized = false; saveNeeded = true; }
    if (req.indexOf("sym=IWM") != -1) { strcpy(settings.symbol, "IWM"); applySymbolPreset("IWM"); initialized = false; saveNeeded = true; }
    if (req.indexOf("mute=1") != -1) { settings.isMuted = true; saveNeeded = true; }
    if (req.indexOf("mute=0") != -1) { settings.isMuted = false; saveNeeded = true; }
    // Expert Tuning
    if (req.indexOf("chop=") != -1) { 
        float nc = req.substring(req.indexOf("chop=")+5).toFloat(); 
        if(nc>0) { settings.chopLimit = nc; saveNeeded = true; }
    }
    // Admin Actions
    if (req.indexOf("test=bull") != -1) { bzDuration = DUR_BULLISH; bzTimer = millis(); bzState = BZ_TONE_1; logEvent("TEST BULL", 0.50); }
    if (req.indexOf("test=bear") != -1) { bzDuration = DUR_BEARISH; bzTimer = millis(); bzState = BZ_TONE_1; logEvent("TEST BEAR", -0.50); }
    if (req.indexOf("reboot=1") != -1) { System.reset(); }
    
    if (saveNeeded) { saveSettings(); forceFullUpdate = true; }

    // --- HTML DASHBOARD GENERATOR ---
    client.println("HTTP/1.1 200 OK\nContent-Type: text/html\nConnection: close\n");
    client.print("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    client.print("<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>");
    client.print("<script src='https://cdn.jsdelivr.net/npm/chartjs-plugin-annotation'></script>");
    client.print("<style>");
    
    // CSS Styling
    client.print("body{font-family:'Segoe UI',sans-serif;background:#121212;color:#eee;margin:0;padding:10px;}");
    client.print(".container{max-width:600px;margin:0 auto;width:100%;box-sizing:border-box;}"); 
    client.print(".card{background:#1e1e1e;padding:15px;border-radius:8px;margin-bottom:15px;border:1px solid #333}");
    client.print("h2{color:#00ff88;font-size:16px;border-bottom:1px solid #333;padding-bottom:5px;margin-top:0}");
    client.print("h3{color:#fff;font-size:14px;margin:10px 0 5px 0;}");
    client.print("p, li{font-size:13px;color:#ccc;line-height:1.4;margin:5px 0}");
    client.print(".stat{font-size:12px;color:#aaa;display:inline-block;margin-right:15px} .val{color:#fff;font-weight:bold}");
    client.print("button{width:100%;padding:15px;margin:5px 0;border-radius:5px;cursor:pointer;border:none;font-weight:bold;color:#fff;background:#333;transition:0.2s}");
    client.print(".gold{background:#FFD700 !important; color:#000 !important; border:2px solid #fff}");
    client.print("input{width:100%;padding:10px;background:#2c2c2c;border:1px solid #444;color:#fff;border-radius:4px;box-sizing:border-box}");
    client.print("</style></head><body><div class='container'>"); 

    // 1. Vitals Card
    client.print("<div class='card'><h2>SYSTEM VITALS</h2>");
    client.print("<div class='stat'>WiFi: <span class='val'>" + String(WiFi.RSSI()) + "dBm</span></div>");
    client.print("<div class='stat'>RAM: <span class='val'>" + String(System.freeMemory()/1024) + "kB</span></div>");
    client.print("<div class='stat'>Uptime: <span class='val'>" + String(System.uptime()/60) + "m</span></div>");
    client.print("</div>");

    // 2. Monitor Card (Graph)
    client.print("<div class='card'><h2>LIVE MONITOR</h2>");
    client.print("<p>SYMBOL: <b id='symText' style='color:#FFD700'>" + String(settings.symbol) + "</b> | PRICE: <b id='priceText'>$" + String(lastPrice, 2) + "</b></p>");
    client.print("<div style='position:relative; height:180px; width:100%'><canvas id='c'></canvas></div></div>");

    // 3. Recent Alerts Card (Auto-populated by JS)
    client.print("<div class='card'><h2>RECENT ALERTS (LAST 5)</h2>");
    client.print("<div id='alertBox' style='min-height:20px;color:#888;font-size:13px'>Waiting for data...</div>");
    client.print("</div>");

    // 4. Presets Card (Gold Buttons)
    String clsSpy = (strcmp(settings.symbol, "SPY") == 0) ? "gold" : "";
    String clsQqq = (strcmp(settings.symbol, "QQQ") == 0) ? "gold" : "";
    String clsIwm = (strcmp(settings.symbol, "IWM") == 0) ? "gold" : "";

    client.print("<div class='card'><h2>PRESETS</h2><div style='display:flex;gap:10px;'>");
    client.print("<a href='/?sym=SPY' style='flex:1'><button class='" + clsSpy + "'>SPY</button></a>");
    client.print("<a href='/?sym=QQQ' style='flex:1'><button class='" + clsQqq + "'>QQQ</button></a>");
    client.print("<a href='/?sym=IWM' style='flex:1'><button class='" + clsIwm + "'>IWM</button></a>");
    client.print("</div></div>");

    // 5. Tuning & Admin Cards
    client.print("<div class='card'><h2>EXPERT TUNING</h2><form action='/' method='get'><div class='stat'>Chop Limit:</div><div style='display:flex;gap:10px'><input type='text' name='chop' value='" + String(settings.chopLimit, 2) + "'><button style='width:auto;background:#00ff88;color:#000'>UPDATE</button></div></form></div>");
    client.print("<div class='card'><h2>ADMIN</h2><a href='/?mute=" + String(settings.isMuted ? "0" : "1") + "'><button>" + String(settings.isMuted ? "UNMUTE" : "MUTE") + " AUDIO</button></a><div style='display:flex;gap:10px'><a href='/?test=bull' style='flex:1'><button style='background:#0f8;color:#000'>TEST BULL</button></a><a href='/?test=bear' style='flex:1'><button style='background:#f44'>TEST BEAR</button></a></div><a href='/?reboot=1'><button style='background:#555;margin-top:10px'>REBOOT DEVICE</button></a></div>");

    // 6. ABOUT SQUAWK BOX (Restored!)
    client.print("<div class='card'><h2>ABOUT SQUAWK BOX</h2>");
    client.print("<h3>Concept & Creation</h3>");
    client.print("<p>Designed and built by <strong>Jason Edgar</strong> in Orillia, Ontario.</p>");
    
    client.print("<h3>Graph Legend</h3>");
    client.print("<p><strong>V = Velocity (Momentum)</strong><br>Difference between Fast and Slow EMAs.</p>");
    client.print("<ul><li><span style='color:#0f8'>Green Line</span> = Bullish Trend</li>");
    client.print("<li><span style='color:#f44'>Red Line</span> = Bearish Trend</li>");
    client.print("<li><span style='color:#0ff'>Cyan Line</span> = Chop / Neutral</li></ul>");

    client.print("<h3>Hardware Specs</h3>");
    client.print("<ul><li><strong>Core:</strong> Particle Photon 2 (Wi-Fi 6)</li>");
    client.print("<li><strong>Display:</strong> 16x2 LCD with I2C Backpack</li>");
    client.print("<li><strong>Audio:</strong> Active Buzzer (Digital Drive)</li>");
    client.print("<li><strong>Control:</strong> Tactile Momentary Switch</li></ul>");
    client.print("</div>");

    // Footer
    client.print("<div style='text-align:center;color:#555;font-size:11px'>SQUAWK BOX v9.0 | " + WiFi.localIP().toString() + "</div></div>"); 
    
    // --- JAVASCRIPT ENGINE ---
    client.print("<script>let chart; const chop=" + String(settings.chopLimit) + "; function update(){ fetch('/data').then(r=>r.json()).then(j=>{ ");
    client.print("document.getElementById('priceText').innerText='$'+j.price.toFixed(2); ");
    
    // JS Logic to parse and color-code alerts
    client.print("let h=''; if(j.alerts.length==0) h='No alerts yet...'; else { for(let a of j.alerts) { let c='#fff'; if(a.e.includes('BULL'))c='#0f8'; if(a.e.includes('BEAR'))c='#f44'; h+=`<div style='border-bottom:1px solid #333;padding:5px 0;font-family:monospace;display:flex;justify-content:space-between'><span><span style='color:#666;margin-right:10px'>${a.t}</span><span style='color:${c};font-weight:bold'>${a.e}</span></span><span style='color:#eee'>${a.v.toFixed(2)}</span></div>`; } } document.getElementById('alertBox').innerHTML=h; ");
    
    // JS Logic to update graph
    client.print("chart.data.datasets[0].data=j.history; let v=j.diff; let color='#0ff'; if(v>chop)color='#0f8'; else if(v<-chop)color='#f44'; chart.data.datasets[0].borderColor=color; chart.update(); }); } window.onload=()=>{ const ctx=document.getElementById('c'); chart=new Chart(ctx,{type:'line',data:{labels:Array(");
    client.print(HISTORY_SIZE);
    client.print(").fill(''),datasets:[{data:[],borderColor:'#0ff',pointRadius:0,tension:0.3}]},options:{maintainAspectRatio:false, responsive:true, plugins:{legend:{display:false},annotation:{annotations:{zero:{type:'line',yMin:0,yMax:0,borderColor:'#FFD700',borderWidth:2},bull:{type:'line',yMin:chop,yMax:chop,borderColor:'#0f8',borderDash:[5,5]},bear:{type:'line',yMin:-chop,yMax:-chop,borderColor:'#f44',borderDash:[5,5]}}}},scales:{y:{grid:{color:'#333'}},x:{display:false}}}}); setInterval(update,3000); update(); };</script></body></html>");

    client.flush(); delay(10); client.stop();
}

// =======================================================================================
// SYSTEM UTILITIES (LCD, LED, AUDIO, BUTTON, PRESETS)
// =======================================================================================

// --- VISUAL OUTPUT ---
void updateLCD() {
    char b0[17]; char b1[17];
    
    // Row 0: "SPY :$500.25"
    snprintf(b0, sizeof(b0), "%s :$%-7.2f", settings.symbol, lastPrice);
    
    // Row 1: "V:0.15  BULL" (Option 3 Logic)
    String status = "CHOP";
    if (diff > settings.chopLimit) status = "BULL";
    else if (diff < -settings.chopLimit) status = "BEAR";
    
    snprintf(b1, sizeof(b1), "V:%-6.2f %s", diff, status.c_str());

    // Only redraw if changed (Prevents flicker)
    if (strcmp(b0, lastRow0_Text) != 0) { lcd.setCursor(0, 0); lcd.print(b0); strcpy(lastRow0_Text, b0); }
    if (strcmp(b1, lastRow1_Text) != 0) { lcd.setCursor(0, 1); lcd.print(b1); strcpy(lastRow1_Text, b1); }

    // Icons (Right Side)
    byte cI = (diff > settings.chopLimit) ? 2 : (diff < -settings.chopLimit ? 3 : 4);
    if (cI != lastIconTrend) { lcd.setCursor(13, 0); lcd.write(cI); lastIconTrend = cI; }
    int cM = settings.isMuted ? 1 : 0;
    if (cM != lastIconMute) { lcd.setCursor(15, 0); lcd.write(cM); lastIconMute = cM; }
}

void updateLED() {
    if (!initialized) { RGB.color(255, 60, 0); return; } // Orange = Booting
    if (diff > settings.chopLimit) { RGB.color(0, 255, 0); } // Green = Bull
    else if (diff < -settings.chopLimit) { RGB.color(255, 0, 0); } // Red = Bear
    else { int v = (millis() % 2000 < 1000) ? 255 : 30; RGB.color(0, 0, v); } // Blue Pulse = Chop
}

// --- AUDIO SEQUENCER ---
void updateBuzzer() {
    if (bzState == BZ_IDLE || settings.isMuted) { digitalWrite(BUZZER_PIN, LOW); return; }
    unsigned long el = millis() - bzTimer;
    switch (bzState) {
        case BZ_TONE_1: // Solid Tone
            digitalWrite(BUZZER_PIN, HIGH); 
            if (el >= bzDuration) { digitalWrite(BUZZER_PIN, LOW); bzState = BZ_IDLE; } 
            break;
        case BZ_TONE_2: // Stutter Tone (Machine Gun)
            if ((el % 80) < 40) digitalWrite(BUZZER_PIN, HIGH); else digitalWrite(BUZZER_PIN, LOW);
            if (el >= 300) { digitalWrite(BUZZER_PIN, LOW); bzState = BZ_IDLE; } 
            break;
        case BZ_GAP: 
            digitalWrite(BUZZER_PIN, LOW); 
            if (el >= DUR_GAP) { bzState = BZ_TONE_2; bzTimer = millis(); } 
            break;
    }
}

// --- SMART POLLING SCHEDULER ---
unsigned long getSmartInterval() {
    if (Time.weekday() == 1 || Time.weekday() == 7) return 300; // Weekend Sleep
    int h = Time.hour(); int m = Time.minute();
    
    // SLEEP (60s): Before 8:30am & After 4:00pm
    if (h < 8 || (h == 8 && m < 30) || h >= 16) return 60; 
    
    // TURBO (2s): 9:30am - 11:00am (Open) & 3:00pm - 4:00pm (Close)
    if ((h == 9 && m >= 30) || h == 10 || h == 15) return 2; 
    
    // LUNCH (10s): 12:00pm - 1:00pm
    if (h == 12) return 10;
    
    // STANDARD (4s): 8:30am-9:29am (Pre) & 11:00am-12:00pm & 1:00pm-3:00pm
    return 4; 
}

// --- INPUT & MEMORY ---
void handleButton() {
    int reading = digitalRead(BUTTON_PIN);
    static unsigned long pressStartTime = 0;
    static bool isPressing = false;
    
    if (reading == LOW && !isPressing) { pressStartTime = millis(); isPressing = true; } 
    else if (reading == HIGH && isPressing) {
        unsigned long duration = millis() - pressStartTime;
        if (duration > 1000) { // Long Press: Backlight
            settings.backlightOn = !settings.backlightOn;
            if(settings.backlightOn) lcd.backlight(); else lcd.noBacklight();
            digitalWrite(BUZZER_PIN, HIGH); delay(50); digitalWrite(BUZZER_PIN, LOW);
        } else if (duration > 50) { // Short Press: Mute
            settings.isMuted = !settings.isMuted; forceFullUpdate = true;
        }
        saveSettings(); isPressing = false;
    }
    lastButtonState = reading;
}

void applySymbolPreset(const char* sym) {
    if (strcmp(sym, "SPY") == 0) { settings.alphaFast = P_SPY_FAST; settings.alphaSlow = P_SPY_SLOW; settings.chopLimit = P_SPY_CHOP; } 
    else if (strcmp(sym, "QQQ") == 0) { settings.alphaFast = P_QQQ_FAST; settings.alphaSlow = P_QQQ_SLOW; settings.chopLimit = P_QQQ_CHOP; }
    else if (strcmp(sym, "IWM") == 0) { settings.alphaFast = P_IWM_FAST; settings.alphaSlow = P_IWM_SLOW; settings.chopLimit = P_IWM_CHOP; }
}

void loadSettings() { 
    EEPROM.get(0, settings); 
    // Version Check: Forces reset if we update the code structure
    if (settings.version != 16) { 
        settings.version = 16; 
        strcpy(settings.symbol, "SPY"); applySymbolPreset("SPY"); 
        settings.isMuted = false; settings.backlightOn = true; 
        saveSettings(); 
    } 
}
void saveSettings() { EEPROM.put(0, settings); }
