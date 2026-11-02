# Maintenance API v1 и SPU1

Все HTTP endpoints доступны на Hub, требуют обычной авторизации владельца и same-origin. JSON: UTF-8, неизвестные поля/вложенный NUL отклоняются; целые байты и смещения ограничены размером выбранного образа. Ошибка имеет обычный Error-конверт API v1. Команды управления исполнительными устройствами не являются частью этого API.

## Растения и обслуживание

- `GET /api/v1/plants`: PlantCatalog из основной [JSON Schema](../../v1/schema.json). Каталог не является списком online-устройств. До 20 растений и 40 исторических привязок; исчерпание не очищает историю молча.
- `PUT /api/v1/nodes/{device_id}/plant`: существующие поля метаданных плюс `room` (UTF-8, 0..80 bytes) и `setup_stage` (0..3). Они передаются вместе. Ревизия защищает от перезаписи параллельных изменений. Доступно и offline.
- `POST /api/v1/plants/{plant_id}/controller`: `{schema_version:1,revision:uint32,device_id:DeviceId}`. Только архивное растение и новый контроллер с исходными метаданными stage=0/revision=0. Plant ID/имя/комната сохраняются, этап становится 2, состояние защитных лимитов новой платы не заменяется старым.
- `POST /api/v1/nodes/{id}/detach` или `/forget`: `{schema_version:1,operation_id:UUIDv4,acknowledge_autonomy:true}`. Detach — только online, поддержка capability bit 32. Forget — только offline, не выключает автономную плату. Без подтверждения владельца отклоняется.
- `GET /api/v1/maintenance`: `{schema_version:1,phase,message,operation_id,device_id}`; nullable ID при отсутствии транзакции. Фазы idle/waiting/unknown/storage_error/completed/unavailable. `POST /maintenance/retry` с `{}` продолжает ту же сохранённую операцию. Потерянный ответ не создаёт новую отвязку.

SPP kind 14 MANAGE: ключи 0=14,3=1 detach,24 UUID операции. Ответ kind15: 0=15,6 status (0 success,1 busy,2 storage error),24 тот же UUID. Node атомарно сохраняет manual mode и отсутствие связи до подтверждения, затем перезапускается. Журнал безопасности не удаляется.

HELLO расширен: key13=1 означает описание оборудования, key1 — версия `(major<<16)|(minor<<8)|patch`, key23 flags: 1 soil ADC настроен,2 tank input настроен,4 pump GPIO настроен,16 OTA,32 safe detach. Отсутствующий descriptor означает неизвестные возможности, а не установленный комплект. SENSOR необязательный key23 calibration: 0 none,1 soil_two_point (только soil kind slot0; это управляющий soil input). Новые kind14/15 и key23 требуют совместимой версии декодера; старые устройства сначала обновляются по USB.

## Обновления

`GET /api/v1/updates`: schema_version=1, phase (idle/uploading/transferring/rebooting/rebooting_node/completed/failed), error (код или пустая строка), active/verifying/validated (boolean), received_bytes/total_bytes (uint32), firmware_version, data_abi=1, profile (c3/n8r2/n16r8), layout (ota-v1/factory), session_id UUID, last_result (none/verifying/validated/rolled_back). Нулевой UUID означает отсутствие сессии. Last_result относится к последнему сохранённому кандидату Hub, а не к любой новой загрузке страницы.

`POST /updates/begin`: `{schema_version:1,device_id:"hub"|DeviceId,profile,size,sha256,header,session_id}`. Header — первые 128 bytes в 256 строчных hex-символах; SHA — 64 lowercase hex. Размер 128..0x200000 для Hub, 128..0x1e0000 для C3. Session UUIDv4 создаётся клиентом. Ответ 202. Только одна операция, ни pairing, ни detach, ни другие RPC не должны находиться в обработке. C3 должен быть online и поддерживать OTA; Node дополнительно проверяет manual/idle/storage.

`POST /updates/chunk`: `{session_id,offset,data}`. Offset целый, data — 1..768 bytes lowercase hex, первый chunk не меньше 128 bytes. Применяется только следующее смещение. Повтор последнего chunk разрешён после проверки совпадения flash, повторная запись не выполняется. Ответ 200. Браузер не повторяет POST автоматически: при неопределённом результате проверяет GET status.

`POST /updates/finish`: `{session_id}`. Проверяются полная длина, SHA и штатная валидация ESP-IDF перед выбором app slot. Ответ 202. Для C3 начинается отдельная передача. `POST /updates/abort` с `{session_id}` отменяет незавершённую сессию; уже выбранный загрузочный образ не отменяется этим запросом. Тайм-аут поступления chunk — 60 секунд. После reboot проверка занимает 30 секунд; независимый reboot timer — 90 секунд. Ожидание подтверждения Node на Hub — 120 секунд после передачи.

При активном OTA блокируются другие HTTP-изменения и запросы remote config/events. GET cached state/metadata остаются доступны. STOP на радио не блокируется; локальные правила полива сохраняются. Загрузка в RAM ограничена chunk, образ C3 размещается во flash staging.

## SPU1, ESP-NOW

Отдельный от SPP бинарный профиль, максимум 248 bytes. Все числа **little endian**. Заголовок 40 bytes: magic `53 50 55 01` (0..3), role (4), type (5), reserved zero (6..7), boot uint32 (8), sequence uint32 (12), target_boot uint32 (16), session UUID16 (20), offset uint32 (36). Затем 0..192 ciphertext bytes и GCM tag16. Type BEGIN1/DATA2/FINISH3/ACK4/ABORT5.

Ключ AES128 = HKDF-SHA256(pair app_key, empty salt, info ASCII `SmartPlantOS/SPU1/AES128`, length16). Nonce `LE32(boot)||LE32(sequence)||LE32(role)`. AAD весь заголовок. Счётчик передачи общий с SPP, но ключ и nonce domain отдельные. Приём требует уже установленного SPP boot epoch, противоположной роли, текущего target_boot и sequence строго больше принятого; только после проверки tag обновляется replay counter. Любое изменение session/offset/target/tag отклоняется. Номер не переносится назад при повторе: повторяется содержимое с новым sequence.

BEGIN payload166: total LE32, profile byte0 (C3), ABI byte1, SHA256 bytes32, header128. DATA — следующие до192 bytes. FINISH/ABORT без payload. ACK payload2: status (0 ok,1 busy/automatic,2 validation/storage,3 rolled_back), stage (1 begin,2 data,3 committed,4 validated after reboot); offset — подтверждённое следующее смещение; у stage4 после перезапуска offset=0, завершение определяется состоянием слота и UUID операции. Stop-and-wait; до12 попыток через700ms. Duplicate DATA сверяется с flash. Новая сессия не продолжает старую после reboot.

ACK stage3 подтверждает выбор нового boot slot, не завершение обновления. Stage4 требует нового boot epoch, сохранённого UUID сессии, штатного VALID и совпадения running partition с сохранённым кандидатом. Откат даже на сборку с тем же номером версии не должен считаться успехом. Повреждённая/незавершённая прошивка не становится загрузочной.

Описание пакета и USB — [UPDATES](../../../docs/UPDATES.md). SHA-256 — контроль целостности, не подпись происхождения. First-use защита от активного MITM остаётся отдельной задачей.
