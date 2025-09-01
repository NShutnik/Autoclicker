# LightClick — лёгкий автокликер для Windows (C++/Win32)

Простой и быстрый автокликер. Пишется на чистом Win32 API (C++), без .NET и сторонних фреймворков. Работает на Windows 10/11, дружит с DPI и не требует установки — один .exe.
[Скачать последнюю версию](https://github.com/NShutnik/LightClick/releases/tag/release_0.1.6)

<img width="583" height="766" alt="image" src="https://github.com/user-attachments/assets/ed8cfa50-af0d-4212-9dce-3c269ac60233" />


## English Description

**LightClick** is a simple and fast auto clicker for Windows, written in pure C++ using the Win32 API (no .NET or third-party frameworks). It runs on Windows 10/11 and is designed to be lightweight, portable, and easy to use.

[Download the latest release](https://github.com/NShutnik/LightClick/releases/tag/release_0.1.6)

**Features:**
- **Interval in ms or CPS** — switch between "ms" (milliseconds) or "CPS" (clicks per second).
- **Double click** mode (with adaptive pause between press/release).
- **Hold button mode** — clicks and holds the mouse button until stopped.
- **Choose mouse button:** Left (LMB), Right (RMB), Middle, **MB4 (X1)**, **MB5 (X2)**.
- **Fixed position** + **point picker button**:
  - Hover your cursor and left-click to capture coordinates automatically;
  - The confirming click is **not passed** to the target window (suppressed via LL-hook).
- **Stop mode:** infinite / **N clicks** / **N seconds**.
- **Random interval jitter** in percentage ±.
- **Hotkey** can be set directly in the UI (`HOTKEY_CLASS`), default is **F6**.
  - Hotkey is displayed on the **Start/Stop (… )** button and updates when changed.
- **Tray icon and minimize to tray**: menu options for "Show/Hide", "Start/Stop", "Exit".
  - Startup parameter **`/tray`** — start minimized to tray.
- **Settings saved** in `LightClick.ini` next to the EXE.
- **Optional autostart** (checkbox in UI) — writes to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- **DPI-friendly** padding and sizes.

> ⚠️ To click on windows running as **administrator**, you must also run the auto clicker **as administrator**.

---

## Возможности

- **Интервал в мс или CPS** — переключатель «мс / CPS».
- **Двойной клик** (с адаптивной паузой между нажатием/отпусканием).
- **Режим удержания кнопки** — нажимает и держит, пока не остановите.
- **Выбор кнопки мыши:** ЛКМ, ПКМ, средняя, **MB4 (X1)**, **MB5 (X2)**.
- **Фиксированная позиция** + **кнопка выбора точки**:
  - наведите курсор и кликните ЛКМ, координаты подхватятся автоматически;
  - подтверждающий клик **не передаётся** в целевое окно (глушится LL-хуком).
- **Режим остановки:** бесконечно / **N кликов** / **N секунд**.
- **Рандомизация интервала (джиттер)** в процентах ±.
- **Горячая клавиша** задаётся прямо в UI (`HOTKEY_CLASS`), по умолчанию **F6**.
  - Хоткей показывается на кнопке **Старт/Стоп (… )** и обновляется при смене.
- **Трей-иконка и сворачивание в трей**: меню «Показать/Скрыть», «Старт/Стоп», «Выход».
  - Параметр запуска **`/tray`** — старт сразу свёрнутым в трей.
- **Сохранение настроек** в `LightClick.ini` рядом с EXE.
- **Опциональный автозапуск** (галочка в UI) — запись в `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- **DPI-friendly** отступы и размеры.

> ⚠️ Для кликов по окнам, запущенным **от администратора**, сам автокликер тоже нужно запускать **от администратора** (из-за UIPI/Integrity Levels).

---

## Сборка

1. Открой **x64 Native Tools Command Prompt**.
2. В корне проекта:
   ```bat
   rc /nologo app.rc
   cl /W4 /O2 /MT AutoClicker.cpp app.res user32.lib gdi32.lib comctl32.lib winmm.lib shell32.lib advapi32.lib /Fe:LightClick.exe
