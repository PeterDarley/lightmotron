""" Package to hold settings"""

from micropython import const       # type: ignore

BOARD = {
    "Type": const("ESP32"),
    "CPU_Frequency": const(240000000),
}

PINS = {
    "LED": const(2),
    "Button": const(0),
    "SCL": const(22),
    "SDA": const(21),
}

WIFI = {
    "SSID": const("my_ssd"),
    "Password": const("my_password"),
    "Blink_on_connect": const(True),
    "Print_on_connect": const(True),
}

I2C = {
    "Blink_on_connect": const(True),
    "Print_on_connect": const(True),
    "Freq": const(400000),
    "IDs": {
        13: const("Compass"),
        104: const("AccelGyro"),
    }
}

ACCEL_GYRO = {
    "Type": const("MPU6050"),
    "Scale_factor": const(131),
}

COMPASS = {
    "Type": const("QMC5883L"),
}

BILLBOARD = {
    "MOSI":       const(23),   # DIN on MAX7219
    "SCK":        const(18),   # CLK on MAX7219
    "CS":         const(5),    # CS/LOAD on MAX7219
    "Num":        const(4),    # number of chained MAX7219 modules
    "Brightness": const(5),    # 0-15
}