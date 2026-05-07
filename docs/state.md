```mermaid
stateDiagram
    [*] --> SETUP
    SETUP --> SEED
    SEED --> SETUP
    SETUP --> IDLE
    IDLE --> AUTH
    AUTH --> OPEN
    AUTH --> CLOSE
    AUTH --> CALIB
    AUTH --> ERASE
    AUTH --> CONFIG
    CONFIG --> IDLE
    OPEN --> IDLE
    CLOSE --> IDLE
    CALIB --> IDLE
    ERASE --> SETUP
    IDLE --> BUTTON
    BUTTON --> SETUP
    BUTTON --> OPEN
    BUTTON --> CLOSE
```
