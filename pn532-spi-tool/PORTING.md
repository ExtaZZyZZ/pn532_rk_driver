# Перенос PN532 SPI Tool на другую плату RK3568

## Что нужно проверить и изменить

---

## 1. SPI устройство

Первым делом проверить какой `/dev/spidev*` доступен на новой плате:

```bash
ls /dev/spidev*
dmesg | grep -i spi
```

Если `/dev/spidev2.0` недоступен — найти нужный и изменить в коде:

```c
// pn532-spi-tool/main.c
#define DEFAULT_DEVICE  "/dev/spidev2.0"  // ← изменить
```

Или передавать через аргумент без изменения кода:
```bash
pn532-spi-tool --device /dev/spidev1.0 info
```

---

## 2. Проверить что SPI включён в DTB

```bash
# На новой плате
cat /proc/device-tree/spi@fe630000/status 2>/dev/null  # SPI2
# Должно быть: okay

# Если disabled — нужно патчить DTB (см. раздел 5)
```

---

## 3. Напряжение на SPI пинах

**КРИТИЧНО:** RK3568 GPIO банки работают на разных напряжениях.

Проверить напряжение пинов SPI на новой плате по схеме или даташиту.

| Напряжение GPIO | Действие |
|----------------|----------|
| 3.3V | Level shifter НЕ нужен — подключать напрямую |
| 1.8V | **Level shifter ОБЯЗАТЕЛЕН** (TXS0104 или аналог) |

На Diasom DD1Q SPI2 пины работают на **1.8V** — нужен level shifter.

Проверить напряжение:
```bash
# Найти к какому банку питания относятся пины SPI
# Смотреть схему платы или даташит RK3568 TRM
```

---

## 4. Пины подключения PN532

На Diasom DD1Q использовались:

| Коннектор | Пин | Функция   |
|-----------|-----|-----------|
| DD10      | 37  | SPI2_MOSI |
| DD10      | 38  | SPI2_MISO |
| DD10      | 36  | SPI2_CLK  |
| DD10      | 34  | SPI2_CS0  |

На новой плате найти свободные SPI пины по схеме и подключить PN532 соответственно.

---

## 5. Если SPI не включён — патч DTB

### 5.1 Извлечь текущий DTB с платы

Сначала найти где лежит DTB:
```bash
# На плате
cat /proc/cmdline | grep dtb
# Или
ls /boot/*.dtb
```

Если DTB в eMMC (как на Diasom):
```bash
# Найти смещение DTB в разделе
grep -boa $'\xd0\x0d\xfe\xed' /dev/mmcblk0p1 | cat -v | head -3
# Запомнить смещение (например 950528)

# Извлечь DTB
dd if=/dev/mmcblk0p1 bs=1 skip=950528 count=200000 of=/tmp/current.dtb
# Скопировать в WSL
scp root@<ip>:/tmp/current.dtb ~/pn532-driver/
```

### 5.2 Найти нужный SPI контроллер в DTS

```bash
# В WSL
dtc -I dtb -O dts -o current.dts current.dtb

# Найти SPI узлы
grep -n 'spi@' current.dts

# RK3568 SPI базовые адреса:
# SPI0 = 0xfe610000
# SPI1 = 0xfe620000
# SPI2 = 0xfe630000
# SPI3 = 0xfe640000
```

### 5.3 Найти pinctrl группу для нужных пинов

```bash
grep -n 'spi2m0\|spi2m1' current.dts
```

Проверить через devmem какой mux активен на пинах:
```bash
# На плате (пример для GPIO2_B группы)
devmem 0xFDC2002C 32   # GPIO2B iomux low
devmem 0xFDC20030 32   # GPIO2B iomux high
```

### 5.4 Изменить DTS

Найти узел нужного SPI контроллера и изменить:
```dts
spi@fe630000 {
    status = "okay";                    /* было: disabled */
    pinctrl-names = "default";
    pinctrl-0 = <&spi2m0_pins &spi2m0_cs0>;  /* нужная pinctrl группа */
    num-cs = <1>;

    pn532@0 {
        compatible = "spidev";
        reg = <0>;
        spi-max-frequency = <5000000>;  /* 5 МГц */
    };
};
```

### 5.5 Пересобрать и прошить DTB

```bash
# В WSL
dtc -I dts -O dtb -W no-unit_address_vs_reg -o patched.dtb current.dts

# Скопировать на плату
scp patched.dtb root@<ip>:/tmp/

# На плате — сделать резервную копию!
cp /tmp/current.dtb /tmp/current.dtb.bak

# Прошить (смещение должно совпадать с найденным в 5.1)
dd if=/tmp/patched.dtb of=/dev/mmcblk0p1 bs=1 seek=950528 conv=notrunc
sync
reboot
```

**ВАЖНО:** неправильный DTB может сломать загрузку. Всегда иметь резервную копию и SD карту с рабочей системой.

---

## 6. Кросс-компиляция

### Требования WSL
```bash
# Установить кросс-компилятор если не установлен
sudo apt install gcc-aarch64-linux-gnu

# Проверить
aarch64-linux-gnu-gcc --version
```

### Сборка
```bash
cd ~/pn532-driver/pn532-spi-tool
make clean && make

# Проверить архитектуру
file pn532-spi-tool
# Должно быть: ELF 64-bit LSB ... ARM aarch64
```

### Деплой на плату
```bash
scp pn532-spi-tool root@<новый_ip>:/usr/local/bin/
```

---

## 7. Перенос конфигурации

### Секретный ключ

**ВАЖНО:** секретный ключ должен быть одинаковым на всех считывателях одной системы — иначе карты выданные на одном не будут работать на другом.

```bash
# Скопировать ключ со старой платы на новую
scp root@<старый_ip>:/etc/pn532/secret.key root@<новый_ip>:/etc/pn532/
```

Или создать новый (тогда все карты нужно перевыпустить):
```bash
mkdir -p /etc/pn532
dd if=/dev/urandom bs=16 count=1 of=/etc/pn532/secret.key
chmod 600 /etc/pn532/secret.key
```

### База пользователей
```bash
# Скопировать базу пользователей
scp root@<старый_ip>:/etc/pn532/users.csv root@<новый_ip>:/etc/pn532/
scp root@<старый_ip>:/etc/pn532/blacklist root@<новый_ip>:/etc/pn532/
```

---

## 8. Настройка автозапуска

```bash
# Создать скрипт демона
cat > /usr/local/bin/pn532-daemon.sh << 'ENDOFFILE'
#!/bin/sh
while true; do
    result=$(pn532-spi-tool --device /dev/spidev2.0 verify --sector-key AABBCCDDEEFF 2>/dev/null | grep "GRANTED")
    if [ -n "$result" ]; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') $result" >> /var/log/pn532.log
    fi
    sleep 1
done
ENDOFFILE

chmod +x /usr/local/bin/pn532-daemon.sh

# Создать systemd сервис
cat > /etc/systemd/system/pn532.service << 'ENDOFFILE'
[Unit]
Description=PN532 NFC Access Control
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/pn532-daemon.sh
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
ENDOFFILE

# Включить и запустить
systemctl daemon-reload
systemctl enable pn532
systemctl start pn532
systemctl status pn532
```

---

## 9. Проверка после переноса

```bash
# 1. SPI устройство доступно
ls /dev/spidev*

# 2. PN532 отвечает
pn532-spi-tool --device /dev/spidev2.0 info
# Ожидаем: PN532 Firmware: IC=0x32  Ver=1.6

# 3. Карта читается
pn532-spi-tool --device /dev/spidev2.0 uid

# 4. Верификация работает
pn532-spi-tool --device /dev/spidev2.0 verify --sector-key AABBCCDDEEFF

# 5. Демон запущен
systemctl status pn532
tail -f /var/log/pn532.log
```

---

## 10. Типичные проблемы

| Проблема | Причина | Решение |
|----------|---------|---------|
| `/dev/spidev*` не существует | SPI не включён в DTB | Патчить DTB (раздел 5) |
| `cannot open /dev/spidev2.0` | Неверный номер устройства | Проверить `ls /dev/spidev*` |
| `info` зависает | Нет физического подключения или нет питания PN532 | Проверить провода |
| `info` возвращает мусор | Неверный bit-reversal | Проверить `sw_lsb_first` в `pn532_open` |
| `verify` — Access DENIED | Карты выданы с другим `secret.key` | Скопировать ключ со старой платы |
| Плата не загружается после DTB | Неверный патч | Восстановить через SD карту или RKDevTool |

---

## Структура файлов проекта

```
/home/rguser/pn532-driver/
├── current.dtb              # оригинальный DTB платы (резервная копия)
├── current.dts              # декомпилированный DTS (справка)
└── pn532-spi-tool/
    ├── main.c               # CLI интерфейс
    ├── pn532_spi.h/c        # SPI транспорт + API
    ├── pn532_proto.h/c      # HSU протокол PN532
    ├── aes128.h/c           # AES-128 шифрование
    ├── users.h/c            # база пользователей
    ├── Makefile             # сборка
    ├── COMMANDS.md          # справочник команд
    └── PORTING.md           # этот файл

На плате:
/usr/local/bin/pn532-spi-tool    # исполняемый файл
/usr/local/bin/pn532-daemon.sh   # скрипт демона
/etc/systemd/system/pn532.service # systemd сервис
/etc/pn532/secret.key            # AES ключ (16 байт)
/etc/pn532/users.csv             # база пользователей
/etc/pn532/blacklist             # заблокированные пользователи
/var/log/pn532.log               # лог событий
```
