# Smart Plant Protocol v1 — инженерный профиль

SPP здесь означает Smart Plant Protocol, не Bluetooth Serial Port Profile. Этот внутренний протокол не является REST API. Его реализуют `sp_wire`, `sp_crypto`, `sp_service`; ESP-NOW находится за отдельным адаптером. Новая несовместимая версия отклоняется целиком. Автоматического согласования разных версий пока нет.

## Кадр и доверие

Все многобайтные поля заголовка — unsigned big endian. Длина кадра 33..249 bytes; полезный CBOR до 217 bytes. Цель обычного сообщения — ≤200 bytes вместе с накладными расходами. Заголовок 16 bytes, GCM tag 16 bytes. Фрагментации нет.

| Offset | Размер | Поле |
|---|---|---|
| 0 | 2 | ASCII `SP` |
| 2 | 1 | version = 1 |
| 3 | 1 | sender role: Node=0, Hub=1 |
| 4 | 4 | boot epoch, ненулевой; commit в NVS до сетевой передачи |
| 8 | 4 | sequence, ненулевой, увеличивается для **каждой** попытки передачи |
| 12 | 4 | target boot epoch; 0 допускается только у HELLO |
| 16 | N | AES-128-GCM ciphertext CBOR |
| 16+N | 16 | authentication tag |

Nonce = `BE32(role) || BE32(boot) || BE32(sequence)` (12 bytes). AAD = все 16 bytes заголовка. Ключ отдельный для каждой пары, не LMK. Отправка прекращается до переполнения sequence/epoch. Запрещено восстанавливать старые счётчики с теми же ключами. Номера sequence могут пропускаться; кадр со старым или равным номером отклоняется. При новом epoch старый epoch становится недопустим и сохраняется в NVS. HELLO подтверждает идентичность, но не позволяет выполнить действие и не означает online. Остальные кадры адресуются текущему boot получателя.

При повторе RPC меняется sequence кадра, но сохраняются command UUID, параметры и deadline. После перезагрузки Node старый кадр команды отсекается по target epoch; повторно сформированный запрос с тем же UUID проверяется по устойчивому журналу команд.

## CBOR

Одна definite-length map, числовые ключи строго возрастают. Числа — uint64 с минимальной кодировкой RFC 8949. Ключи 0..23 имеют целочисленные значения, 24..27 — byte strings. Floats, negative integers, строки, теги, indefinite-length, неизвестные ключи, дубликаты и хвостовые данные запрещены. Это ограниченный профиль CBOR; общий CBOR-клиент должен использовать canonical encoding.

Для конкретного типа сообщения разрешается только оговорённый набор полей. Идентичность команды — UUID + действие + объём + целевой UUID Stop; deadline не является частью идентичности. UUID команд — v4/RFC variant, 16 bytes. Device ID — 6 случайных bytes, внешний вид `spo-` + 12 hex; он **не** является MAC. Plant ID — 16 bytes UUID.

### Типы сообщений (ключ 0)

| Тип | Значение | Направление и поля |
|---|---|---|
| HELLO | 1 | оба: 0,26 device ID,27 plant ID,23 capability bits; target epoch=0 |
| STATE | 2 | Node→Hub: 0,2,7..16,20..22; 24 только при running |
| TIME | 3 | Hub→Node: 0,2 UTC milliseconds |
| COMMAND | 4 | Water: 0,3=1,4 ml,5 deadline,24 UUID. Stop: 0,3=2,24 новый UUID,25 UUID полива |
| RESULT | 5 | Node→Hub: 0,3 action,4 requested ml (Stop=0),6 status,7 reason,17 started,18 finished,19 estimated µl,24 UUID |
| QUERY | 6 | Hub→Node: 0,24 UUID; ответ RESULT. Неизвестный ID: только 0,23=1,24 |
| CONFIG | 7 | Hub→Node: 0,1 expected revision,3 automatic,4 portion,8 dry,9 wet,10 threshold,11 daily,12 maxrun,13 interval,14 maxage,19 flow,24 request UUID |
| CONFIG_RESULT | 8 | Node→Hub: 0,13 current revision,23=0 success/1 rejected,24 request UUID |
| INFO | 9 | Node→Hub: 0,23=0 time accepted/1 rejected; не подтверждает команду полива |
| EVENT_QUERY | 10 | Hub→Node: 0,21 after sequence |
| EVENT | 11 | Node→Hub: 0,2 UTC,3 event kind,6 status,7 reason,14 uptime,20 source,21 seq,23=0,24 command UUID или 16 нулей |

EVENT с `23=1` означает cursor_expired, `23=2` — новых событий нет. В обоих случаях передаются только 0,21 последняя seq,23. При gap Hub явно сообщает потерю истории и запрашивает оставшиеся события с `max(0,last_seq-32)`. Одно событие на запрос, следующая seq сохраняет порядок. История Hub RAM теряется после reboot, курсор восстанавливается с нуля, возможный gap не скрывается.

CONFIG — полная управляющая конфигурация; проверяется атомарно. Изменения запрещены во время полива и ожидания отклика почвы, чтобы смена калибровки не исказила проверку. Повтор после потерянного ответа считается успешным, если revision ровно expected+1 и все значения совпадают. Иначе несовпадение revision — отказ. UUID ответа позволяет Hub не принять запоздавший результат другой конфигурации. Локальный `status` печатает CONFIG-снимок без UUID: это диагностическое представление, не допустимый запрос применения.

### Поля STATE и единицы

| Ключ | Значение |
|---|---|
| 2 | текущее UTC ms; 0 = clock unset |
| 7 | текущая причина решения, таблица ниже |
| 8 | медиана сырого ADC, 0..4095; качество отдельно |
| 9 | сглаженный калиброванный индекс 0..100; при невалидном качестве не выдавать за достоверный |
| 10 | quality: missing=0, uncalibrated=1, ok=2, fault=3, stale=4 |
| 11 | бак: unknown=0, empty=1, present=2 |
| 12 | pump: off=0, running=1; состояние ПО, не датчик тока насоса |
| 13 | revision управляющей конфигурации uint32 |
| 14 | uptime ms |
| 15 | возраст почвенного измерения ms |
| 16 | защитный резерв ml, **не** daily_estimated_ml |
| 20 | manual=0 / automatic_threshold=1 |
| 21 | последняя устойчивая event seq |
| 22 | внутренний разброс ADC, отсчёты |
| 24 | активный command UUID, отсутствует при off |

CONFIG: raw dry/wet — отсчёты 0..4095, threshold — индекс 1..95, portion — 10..300 ml, daily — portion..1000 ml, maxrun — 1..30 s, interval — 60..10080 min, maxage — 30..900 s, flow — 0..1000000 µl/s. Нулевая калибровка — отсутствие калибровки. Порция должна помещаться в maxrun при текущей производительности. Расширение диапазонов API не обязывает драйвер принимать их все.

RESULT status: accepted=0, running=1, completed=2, stopped=3, failed=4, rejected=5. Нулевые started/finished означают отсутствие достоверного UTC. У отклонённого Water estimated в публичном JSON должен быть null, даже если внутренний счётчик равен нулю. Stop должен работать без UTC; согласование его результата с условиями публичной JSON Schema — обязательная задача перед REST-релизом.

EVENT kind: boot=1, config=2, command=3, latched fault=4, physical acknowledgement=5. Source: device=0, user=1, automatic=2. Для JSON `event_id = device_id + ':' + seq`. Boot/ack и потеря датчика пока не являются полным набором публичных Event; адаптер должен определить отображение и расширить схемы с примерами, не выдумывать uptime/UTC/источник.

Capability bits HELLO: bit0 soil, bit1 tank, bit2 pump. Профиль Node объявляет все три; отсутствие калибровки или назначения GPIO не скрывается за capabilities — это отдельное состояние готовности. Hub объявляет 0. Ключ 1 зарезервирован для корреляции/expected revision, прочие неиспользуемые комбинации отклоняются.

### Причины

0 ready (в публичном JSON reason_code=null); 1 soil_not_dry; 2 sensor_stale; 3 sensor_fault; 4 sensor_uncalibrated; 5 tank_empty; 6 tank_unknown; 7 pump_uncalibrated; 8 cooldown_active; 9 daily_limit; 10 single_limit; 11 pump_busy; 12 clock_unset; 13 hardware_fault; 14 config_invalid; 15 command_expired; 16 command_conflict; 17 storage_fault; 18 watering_no_response. Числовые значения закреплены, нельзя переставлять enum без новой версии SPP.

## Доставка и масштабирование

Максимум 10 привязанных Node, encrypted unicast; broadcast-команды не используются. Регистрировать peer и отправлять ему команды можно только после физического provisioning. Online означает валидный STATE не старше 15000 ms; повреждённый кадр и HELLO срок не продлевают. Hub хранит последнее состояние для диагностики, но будущий REST обязан отвечать `503 node_offline` на запрос текущего состояния offline-устройства.

Один активный RPC на peer. Water имеет deadline 10 s в консоли; Node допускает не более 30 s в будущем. Hub делает до трёх попыток через 1500 ms, затем сообщает неизвестный результат. Stop может заменить незавершённый RPC. Повторять Water с новым UUID запрещено; используется QUERY прежнего UUID. При недоступном Node команды полива не сохраняются в отложенную очередь.

Настройки, история и транспорт не создают общий глобальный контроллер полива: у каждого Node остаются собственные правила и бюджеты. Другой транспорт впоследствии реализует отправку/приём `sp_message_t` и защищённую привязку, сохраняя эти правила.

## Векторы и проверки

[vectors.json](vectors.json) содержит JSON map с числовыми ключами и канонический CBOR hex. Для byte strings используется объект `{"hex":"..."}`. [Инструмент](../../../tools/spp_inspect.py) декодирует консольный hex и проверяет векторы. Соответствие C-кодека проверяется в native tests. Формат AEAD проверяется тем же C-кодом на хосте с Mbed TLS и независимым [вектором](aead-vector.json); нулевой ключ в нём предназначен исключительно для теста.
