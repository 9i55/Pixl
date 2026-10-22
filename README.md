<p align="center">
  <img src="res/pixl.png" width="96" alt="Pixl">
</p>

<h1 align="center">Pixl</h1>

<p align="center">
  Voice effects, a program mixer and a soundboard for Discord and games on Windows.<br>
  <b><a href="../../releases/latest">⬇ Download the latest version</a></b>
</p>

---

<div dir="rtl">

## 🇸🇦 بالعربي

**Pixl** برنامج مجاني لويندوز يغيّر صوتك ويضيف عليه مؤثرات، ويوزّع أصوات البرامج زي الفويس ميتر، وفيه ساوندبورد. يشتغل مع الديسكورد والألعاب وأي برنامج يستخدم المايك.

### المميزات

- **مؤثرات الصوت:** صدى مكان (Concert Hall وCathedral وHaunted Cavern وغيرها)، وتغيير الصوت (Voice mask)، وعزل الضوضاء، وإيكولايزر 7 نطاقات.
- **المكسر (Mixer):** حط برامجك في أعمدة (Audio 1 و 2 و 3)، ولكل عمود:
  - **Fader** للصوت بالـ dB (من ‎-60 إلى 0)، مع عدّاد يمشي مع صوت البرنامج نفسه.
  - **Audio:** تسمع البرنامج أو لا.
  - **Mic:** يطلع صوت البرنامج في مايكك.
  - **Echo:** يضيف صدى على صوت البرنامج.
  - **Solo** و **Mute**.
- **الساوندبورد:** 12 Pad، ولكل واحد مفتاح يشغّله حتى وأنت داخل لعبة. اسحب الأصوات وأفلتها على الـ Pads، أو اسحب ملفات من سطح المكتب مباشرة.
- **المسجّل:** يحتفظ بآخر دقائق من صوتك.

### التحميل والتثبيت

1. افتح صفحة **[آخر إصدار](../../releases/latest)** وحمّل ملف **`Pixl.zip`**.
2. فك الضغط في مجلد ثابت، مثل `Documents\Pixl`.
3. شغّل **`Pixl.exe`**.
   - إذا طلع لك **"Windows protected your PC"**، اضغط **More info** ثم **Run anyway**. التحذير يطلع لأن البرنامج غير موقّع رقميًا.
4. أول مرة، البرنامج يطلب تثبيت **VB-CABLE**، وهو المايك الوهمي اللي يوصل صوتك المعدّل للديسكورد. وافق، وأعد تشغيل الجهاز بعدها.

### الإعداد مع الديسكورد

1. في Pixl اختر **مايكك الحقيقي** في Microphone، و**سماعتك** في Speakers.
2. في الديسكورد افتح **User Settings ← Voice & Video**.
3. خلّ **Input Device** على **CABLE Output** (أو **What U Hear**).
4. اطفِ **Noise Suppression** في الديسكورد، لأنه أحيانًا يخرّب المؤثرات.

### استخدام المكسر

1. اضغط زر **Mixer** فوق في البرنامج.
2. في أي عمود اضغط **+ Add program** واختر البرنامج.
3. استخدم الأزرار:

| Audio | Mic | أنت تسمعه؟ | يطلع في المايك؟ |
|---|---|---|---|
| ✅ | ❌ | ✅ | ❌ |
| ✅ | ✅ | ✅ | ✅ |
| ❌ | ✅ | ❌ | ✅ |
| ❌ | ❌ | ❌ | ❌ |

- **Mute** يكتم العمود كله، من السماعة والمايك.
- **عجلة الماوس** على العمود تغيّر الصوت 1 dB، ومع **Shift** تغيّره 0.1 dB.
- **دبل كليك** على الـ Fader يرجّعه 0 dB.

### المتطلبات

- Windows 10 أو 11 (64-bit). ميزة التقاط صوت البرامج تحتاج Windows 10 إصدار 2004 أو أحدث.
- مايك.
- [VB-CABLE](https://vb-audio.com/Cable/)، والبرنامج يثبّته لك.

</div>

---

## 🇬🇧 English

**Pixl** is a free Windows app that puts effects on your voice, mixes the sound of your programs like Voicemeeter, and has a soundboard. It works with Discord, games and anything else that uses a microphone.

### Features

- **Voice effects:** room echo (Concert Hall, Cathedral, Haunted Cavern and more), voice masks, noise isolation and a 7-band equalizer.
- **Mixer:** put programs on strips (Audio 1, 2, 3). Each strip has:
  - a **fader** in dB (-60 to 0) and a meter that follows the program's own sound,
  - **Audio:** hear the program or not,
  - **Mic:** send the program into your mic,
  - **Echo:** add the room echo to the program,
  - **Solo** and **Mute**.
- **Soundboard:** 12 pads, each with a key that plays it even inside a game. Drag sounds onto pads, or drop sound files straight from Explorer.
- **Recorder:** keeps the last few minutes of your voice.

### Download and install

1. Open the **[latest release](../../releases/latest)** and download **`Pixl.zip`**.
2. Unzip it into a folder that stays, such as `Documents\Pixl`.
3. Run **`Pixl.exe`**.
   - If **"Windows protected your PC"** shows up, click **More info**, then **Run anyway**. It appears because the app isn't code-signed.
4. On first start Pixl offers to install **VB-CABLE**, the virtual microphone that carries your voice to Discord. Accept it, then restart your PC.

### Set up Discord

1. In Pixl, pick your real microphone under **Microphone** and your headphones under **Speakers**.
2. In Discord, open **User Settings → Voice & Video**.
3. Set **Input Device** to **CABLE Output** (or **What U Hear**).
4. Turn off Discord's **Noise Suppression**; it can fight the effects.

### Using the mixer

1. Click **Mixer** at the top of Pixl.
2. In a strip, click **+ Add program** and pick a program.
3. Use the buttons:

| Audio | Mic | You hear it | On the mic |
|---|---|---|---|
| on | off | yes | no |
| on | on | yes | yes |
| off | on | no | yes |
| off | off | no | no |

- **Mute** silences the whole strip, for you and on the mic.
- The **mouse wheel** over a strip moves it 1 dB (0.1 dB with **Shift**).
- **Double-click** a fader to put it back at 0 dB.

### Requirements

- Windows 10 or 11, 64-bit. Capturing a program's sound needs Windows 10 version 2004 or newer.
- A microphone.
- [VB-CABLE](https://vb-audio.com/Cable/). Pixl installs it for you.

### Build from source

You need Visual Studio 2022 (or newer) with the **Desktop development with C++** workload.

```bat
msbuild Pixl.sln /p:Configuration=Release /p:Platform=x64
```

The program is written to `build\MasonEAX\Pixl.exe`.

---

## Credits

Pixl is based on [OctopusEAX](https://github.com/ALMASONYH/OctopusEAX) by ABOLHB / ALMASONYH, released under the MIT License. Pixl adds the program mixer, the soundboard drag and drop, and a new look, and keeps the same license. See [LICENSE](LICENSE).
