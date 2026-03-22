# Neopixel Strip Wiring Diagram

## Wiring Setup

This diagram shows how to wire a neopixel strip to an ESP32 using a 5V power supply with a capacitor, resistor, and logic level converter.

```mermaid
graph TB
    PSU_POS["5V Power Supply<br/>(+5V)"]
    PSU_GND["Common Ground"]
    
    subgraph ESP["ESP32"]
        ESP_GND["GND"]
        ESP_GPIO["GPIO Pin<br/>(e.g., 4 or 5)"]
    end
    
    subgraph LLC["Logic Level<br/>Converter"]
        LLC_LV_GND["LV GND"]
        LLC_LV_IN["LV IN"]
        LLC_HV_GND["HV GND"]
        LLC_HV_VCC["HV VCC"]
        LLC_HV_OUT["HV OUT"]
    end
    
    subgraph NEOPIXEL["Neopixel Strip"]
        NEO_VCC["VCC (+5V)"]
        NEO_GND["GND"]
        NEO_DIN["DIN"]
    end
    
    CAP["Capacitor 1000µF<br/>between 5V and GND<br/>Place close to<br/>neopixel power pins"]
    RES["Resistor 470Ω"]
    
    %% Power rail
    PSU_POS -->|5V Power| NEO_VCC
    PSU_POS -->|5V Power| LLC_HV_VCC
    PSU_POS -->|5V Power| CAP
    CAP -->|GND| PSU_GND
    
    %% Ground rail
    PSU_GND -->|Common Ground| NEO_GND
    PSU_GND -->|Common Ground| ESP_GND
    PSU_GND -->|Common Ground| LLC_LV_GND
    PSU_GND -->|Common Ground| LLC_HV_GND
    
    %% ESP32 to Logic Level Converter
    ESP_GPIO -->|3.3V Signal| LLC_LV_IN
    
    %% Logic Level Converter to Neopixel
    LLC_HV_OUT -->|5V Signal| RES
    RES -->|Protected Signal| NEO_DIN
    
    style PSU_POS fill:#ff9999
    style PSU_GND fill:#ff9999
    style ESP fill:#99ccff
    style LLC fill:#99ff99
    style NEOPIXEL fill:#ffcc99
    style CAP fill:#ff99ff
    style RES fill:#ffff99
```

## Key Points

- **All grounds must be connected together** (common ground)
- The **capacitor** (1000µF recommended) is placed close to the neopixel strip's power pins for voltage stability
- The **resistor** (470Ω recommended) sits between the logic level converter output and the neopixel DIN to protect against signal issues
- The **logic level converter** bridges the 3.3V ESP32 signal to the 5V neopixel requirements

## Component Details

| Component | Purpose |
|-----------|---------|
| Capacitor | Reduces voltage spikes and provides stable power |
| Resistor | Protects against signal reflections |
| Logic Level Converter | Converts 3.3V ESP32 signal to 5V neopixel signal |

Make sure to use a 5V power supply with sufficient amperage for your LED count (roughly 60mA per LED at full brightness).
