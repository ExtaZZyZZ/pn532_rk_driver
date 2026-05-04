# PN532 SPI Tool — Справочник команд

## Устройство и ключи (общие опции)

| Опция | По умолчанию | Описание |
|-------|-------------|----------|
| `--device PATH` | `/dev/spidev2.0` | Путь к SPI устройству |
| `--secret PATH` | `/etc/pn532/secret.key` | AES-128 ключ шифрования |
| `--sector-key HEXKEY` | `FFFFFFFFFFFF` | 6-байтный ключ сектора карты |
| `--key HEXKEY` | `FFFFFFFFFFFF` | Ключ для read/write |
| `--key-b` | — | Использовать Key B вместо Key A |

---

## Диагностика

### info — версия прошивки PN532
```bash
pn532-spi-tool --device /dev/spidev2.0 info
```
Проверяет что PN532 подключён и отвечает.
```
PN532 Firmware: IC=0x32  Ver=1.6
```

### uid — прочитать UID карты
```bash
pn532-spi-tool --device /dev/spidev2.0 uid
```
Показывает уникальный идентификатор карты.
```
Card UID: 7E 5B CA 06
```

---

## Работа с блоками карты

### read — прочитать блок
```bash
pn532-spi-tool --device /dev/spidev2.0 read --block 4
pn532-spi-tool --device /dev/spidev2.0 read --block 4 --key AABBCCDDEEFF
pn532-spi-tool --device /dev/spidev2.0 read --block 4 --key AABBCCDDEEFF --key-b
```
Читает 16 байт из указанного блока. Требует аутентификации.
```
Card UID: 7E 5B CA 06
Authentication OK
Block 4: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```
> Блоки карты: 0-3 = сектор 0, 4-7 = сектор 1, 8-11 = сектор 2 и т.д.

### write — записать блок
```bash
pn532-spi-tool --device /dev/spidev2.0 write --block 4 --data 0102030405060708090A0B0C0D0E0F10
pn532-spi-tool --device /dev/spidev2.0 write --block 4 --data 0102030405060708090A0B0C0D0E0F10 --key AABBCCDDEEFF
```
Записывает ровно 16 байт (32 hex символа) в блок.
> Блок 0 (UID) и sector trailers (3,7,11...) защищены от записи.

---

## Система контроля доступа

### enroll — записать карту пользователю
```bash
pn532-spi-tool --device /dev/spidev2.0 enroll --user-id 42 --sector-key AABBCCDDEEFF
```
Привязывает карту к пользователю:
1. Читает UID карты
2. Создаёт токен: `UID + user_id` зашифрованный AES-128
3. Записывает токен в блок 4
4. Меняет ключ сектора на `--sector-key`

Делается **один раз** при выдаче карты пользователю.
```
Card UID: 7E 5B CA 06
Enroll OK: user_id=42 written to card
```

### verify — проверить карту
```bash
pn532-spi-tool --device /dev/spidev2.0 verify --sector-key AABBCCDDEEFF
pn532-spi-tool --device /dev/spidev2.0 verify --sector-key AABBCCDDEEFF --secret /etc/pn532/secret.key
```
Проверяет подлинность карты:
1. Читает UID карты
2. Читает и расшифровывает токен с блока 4
3. Сравнивает UID из токена с реальным UID
4. Проверяет blacklist
5. Ищет пользователя в базе

```
Access GRANTED: user_id=42 name=Ilya Yakovlev role=admin
```
или
```
Access DENIED: card token invalid or cloned
```
**Exit code:** `0` = доступ разрешён, `1` = отказ.

---

## База пользователей

Файл: `/etc/pn532/users.csv` (формат: `id,name,role`)

### user-add — добавить пользователя
```bash
pn532-spi-tool user-add --user-id 42 --name "Ilya Yakovlev" --role admin
pn532-spi-tool user-add --user-id 43 --name "Egor Gromov" --role user
```
```
User added: id=42 name=Ilya Yakovlev role=admin
```

### user-del — удалить пользователя
```bash
pn532-spi-tool user-del --user-id 42
```
Удаляет из базы и **автоматически добавляет в blacklist**.
```
User 42 deleted
```

### user-list — список пользователей
```bash
pn532-spi-tool user-list
```
```
ID       Name                             Role
-------- -------------------------------- ----------------
42       Ilya Yakovlev                    admin
43       Egor Gromov                      user

Total: 2 user(s)
```

---

## Blacklist (блокировка карт)

Файл: `/etc/pn532/blacklist` (один user_id на строку)

### blacklist-add — заблокировать пользователя
```bash
pn532-spi-tool blacklist-add --user-id 42
```
Карта пользователя будет отклоняться при verify.
```
User 42 added to blacklist
```

### blacklist-del — разблокировать пользователя
```bash
pn532-spi-tool blacklist-del --user-id 42
```
```
User 42 removed from blacklist
```

### blacklist-list — показать blacklist
```bash
pn532-spi-tool blacklist-list
```
```
Blacklisted user IDs:
  42
  43
```

---

## Типичные сценарии

### Выдача карты новому сотруднику
```bash
# 1. Добавить в базу
pn532-spi-tool user-add --user-id 50 --name "Ivan Petrov" --role user

# 2. Записать карту (поднести карту к считывателю)
pn532-spi-tool --device /dev/spidev2.0 enroll --user-id 50 --sector-key AABBCCDDEEFF
```

### Блокировка уволенного сотрудника
```bash
pn532-spi-tool user-del --user-id 50
# или только заблокировать без удаления из базы:
pn532-spi-tool blacklist-add --user-id 50
```

### Восстановление доступа
```bash
pn532-spi-tool blacklist-del --user-id 50
```

### Запуск в цикле (скрипт-демон)
```bash
while true; do
    result=$(pn532-spi-tool --device /dev/spidev2.0 verify --sector-key AABBCCDDEEFF 2>/dev/null | grep "GRANTED")
    if [ -n "$result" ]; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') $result" >> /var/log/pn532.log
    fi
    sleep 1
done
```

### Просмотр лога
```bash
# В реальном времени
tail -f /var/log/pn532.log

# Все события конкретного пользователя
grep "Ilya Yakovlev" /var/log/pn532.log

# Количество визитов пользователя 42
grep "user_id=42" /var/log/pn532.log | wc -l
```

---

## Файлы системы

| Файл | Назначение |
|------|------------|
| `/etc/pn532/secret.key` | AES-128 ключ (16 байт, бинарный) — **хранить в тайне!** |
| `/etc/pn532/users.csv` | База пользователей |
| `/etc/pn532/blacklist` | Заблокированные user_id |
| `/var/log/pn532.log` | Лог событий |
| `/usr/local/bin/pn532-spi-tool` | Исполняемый файл |
| `/usr/local/bin/pn532-daemon.sh` | Скрипт демона |
| `/etc/systemd/system/pn532.service` | Systemd сервис |
