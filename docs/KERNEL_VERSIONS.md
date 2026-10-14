# PhotonOS — хранение ядер в /sys/kernel и версионирование

## Цель

Перенести `kernel.bin` (ранее `init.bin` на фиксированном LBA 60) внутрь
локальной файловой системы DATSLAF по пути `/sys/kernel`, чтобы:

- упростить установку и обновление ядра без перепрошивки всего образа,
- хранить несколько версий ядра на одном диске,
- дать возможность загрузчику выбирать версию при старте,
- подготовить основу для будущего установщика ОС.

## Текущая реализация

### 1. Сборка

В `Makefile`:

- `KERNEL_FS_DIR := $(FS_SOURCE_DIR)/sys/kernel`
- цель `build_vd` теперь зависит от `init.bin` и перед сборкой `.vd` образа:
  - создаёт `$(KERNEL_FS_DIR)`,
  - копирует `init.bin` как `kernel.bin`,
  - дополнительно как `kernel-1.0.0.bin` и `kernel-$(DATE).bin`,
  - создаёт файлы `boot.cfg`, `default`, `active` с содержимым `kernel.bin`,
  - создаёт `versions.txt` и `README.txt` с описанием.

Таким образом, в `fs.vd` уже есть ядро. Старый регион `init.bin` на LBA 60
пока оставлен для обратной совместимости (fallback).

- `IMG_STAGE2_SECTORS` увеличен с 15 до 31 (7.5 КБ → 15.5 КБ),
  `IMAGE_HEAD_BYTES` в `boot/bootinfo.inc` — с 8192 до 16384,
  чтобы вместить расширенный stage2 с поддержкой FS.

### 2. Загрузчик stage2 (`boot/stage2.asm`)

Новый stage2:

1. Читает суперблок DATSLAF по `FS_LBA` (из заголовка образа) в bounce-буфер,
   проверяет магию `0xDA751AF5`.
2. Загружает таблицу узлов (`FS_LBA+1`) в память по `0xC000`.
3. Ищет корень (`parent_id == -1`), затем `/sys`, затем `/sys/kernel`.
4. Пытается прочитать `boot.cfg` / `default` / `active` из `/sys/kernel` —
   если файл найден, первая строка считается именем ядра для загрузки.
5. Если конфига нет — пробует `kernel.bin`.
6. Если и его нет — перечисляет все файлы в `/sys/kernel`
   (кроме мета-файлов `boot.cfg`, `default`, `active`, `versions.txt`),
   показывает меню:

```
Available kernels in /sys/kernel:
  0) kernel.bin
  1) kernel-1.0.0.bin
  2) kernel-Oct_14_2026_18-21.bin
Select kernel [0-9] or Enter for default:
```

   Выбор — клавишей `0-9` или `Enter` (default 0).

7. Загружает выбранный файл:
   `first_sector = FS_LBA + data_block`, `sectors = ceil(size/512)`,
   `dest = KERNEL_ADDR (0x100000)` через существующий `load_image_region`
   (с учётом `sector_shift` для CD/HDD).

8. При успехе обновляет `bootinfo.kernel_sectors` и продолжает как раньше
   (копия образа в ramdisk, переход в protected mode).

9. При неудаче — fallback на старый метод загрузки по фиксированному
   LBA `IH_INIT_LBA`.

Память для FS-загрузчика:

- `FS_NODES_ADDR 0xC000` — таблица узлов (~7 КБ, <64 КБ для доступа через SI)
- `FS_LIST_ADDR 0xE000` — список ID ядер
- `FS_TMP_NAME_ADDR 0xE400` — имя из boot.cfg
- `FS_BOOTCFG_BUF 0xE600` — содержимое boot.cfg
- `BOUNCE_ADDR 0x10000` — как и раньше для BIOS чтений

Все адреса ниже 64 КБ (кроме bounce), чтобы `puts`/`lodsb` работали в
real mode с DS лимитом 4 ГБ после первого `unreal_mode`.

### 3. Ядро — команды управления

Встроенная оболочка ядра (`kernel/kernel.c`) теперь имеет:

- `klist` / `kver` / `kernels` — список ядер в `/sys/kernel`
  (помечает `[kernel]` и `[meta]`, показывает `boot.cfg` и текущую версию).
- `kset <file>` — установить ядро по умолчанию:
  перезаписывает `boot.cfg`, `default`, `active` содержимым `<file>`
  и вызывает `fs_flush_node` для сохранения на диск.
- `kinstall <src_path> <dest_name>` — установить новое ядро:
  копирует файл из ФС (например `/home/new.bin`) в `/sys/kernel/<dest_name>`.
- `kinfo` — версия ядра, build date, наличие `/sys/kernel`, `lsblk`.

Пример сессии:

```
> klist
Kernels in /sys/kernel:
  [kernel] kernel.bin (57680 bytes)
  [kernel] kernel-1.0.0.bin (57680 bytes)
  [meta] boot.cfg (11 bytes)
Default (boot.cfg): kernel.bin
Current running: kernel 1.0.0

> kinstall /home/mytest.bin kernel-1.1.0.bin
Installed /sys/kernel/kernel-1.1.0.bin (58200 bytes)

> kset kernel-1.1.0.bin
Default kernel set to kernel-1.1.0.bin
It will be used on next boot

> klist
  [kernel] kernel.bin
  [kernel] kernel-1.0.0.bin
  [kernel] kernel-1.1.0.bin
Default (boot.cfg): kernel-1.1.0.bin
```

### 4. Файловая система

- `kernel/datslaf/root/sys/kernel/` теперь часть исходников (с `README.txt`).
- При `make clean` генерируемые `kernel*.bin`, `boot.cfg`, `default`,
  `active`, `versions.txt` удаляются, `README.txt` остаётся.
- `fs.vd` вырос примерно на размер ядра (~60 КБ → ~350 КБ), всё ещё
  помещается в образ 2 МБ (FS регион 384..4095 = 3712 секторов = 1.8 МБ).

### 5. Совместимость

- Старые образы без `/sys/kernel` продолжают грузиться через LBA.
- Новые образы грузятся через FS, но при повреждении FS — fallback на LBA.
- Для USB/CD загрузка всё равно идёт через ramdisk, но теперь ramdisk
  уже содержит `/sys/kernel` с ядрами, так что выбор версии работает и там.

## Будущий установщик

С текущим layout установщик может:

1. Разметить диск, создать раздел типа `0xDA`, записать MBR + stage2.
2. Скопировать `fs.vd` (который уже содержит `/sys/kernel/kernel.bin`).
3. При обновлении:
   - скопировать новый `kernel-X.Y.Z.bin` в `/sys/kernel/`,
   - обновить `boot.cfg`,
   - оставить старые версии для отката.

Прототип скрипта `tools/kern_install.py`:

- принимает путь к `os-image.bin` или `fs.vd` и новый `kernel.bin`,
- монтирует DATSLAF (через `build_fs.py` логику),
- добавляет файл в `/sys/kernel/`,
- пересобирает образ.

См. `tools/kern_install.py` и `tools/mkimage.py`.

## Ограничения и TODO

- stage2 меню поддерживает только 0-9 (до 10 ядер) — можно расширить до
  ввода числа с Enter.
- Нет проверки подписи ядра — в будущем добавить проверку SHA256.
- `kinstall` пока работает только внутри запущенной ОС (RAM FS);
  для USB/CD изменения теряются после перезагрузки — нужен USB стек
  или установка на HDD.
- Установщик пока прототип — нужно добавить поддержку разметки,
  копирования всего образа, а не только ядра.

## Связанные файлы

- `boot/bootinfo.inc` — увеличен `IMAGE_HEAD_BYTES` до 16384
- `boot/stage2.asm` — полностью переработан, добавлен FS-лоадер
- `Makefile` — `IMG_STAGE2_SECTORS=31`, `build_vd` копирует ядро в FS
- `kernel/kernel.c` — команды `klist`, `kset`, `kinstall`, `kinfo`
- `kernel/datslaf/root/sys/kernel/README.txt` — документация внутри FS
- `docs/KERNEL_VERSIONS.md` — этот файл
