<div align="center">

<img src="docs/images/store/icon.png" width="128" alt="Dashdance app icon">

# Dashdance

**Melee online, native on Mac, iPhone, iPad and Apple Vision Pro.**

Games &nbsp;·&nbsp; Free &nbsp;·&nbsp; Open source &nbsp;·&nbsp; For Slippi Online

<a href="#get-dashdance"><img src="docs/images/store/get.png" height="52" alt="Get Dashdance"></a>

<br><br>

<img src="docs/images/store/hero.jpg" width="880" alt="Dashdance on a MacBook and an iPhone">

<br>

<img src="docs/images/store/card-1-native.jpg" width="19%" alt="Not emulated. Native.">
<img src="docs/images/store/card-2-online.jpg" width="19%" alt="Online, built in.">
<img src="docs/images/store/card-3-controller.jpg" width="19%" alt="Your controller, your way.">
<img src="docs/images/store/card-4-connect.jpg" width="19%" alt="Connect in seconds.">
<img src="docs/images/store/card-5-menu.jpg" width="19%" alt="Settings, mid-match.">

<sub>Dashdance is an independent community app. It is not affiliated with or endorsed by the Slippi team or Nintendo.</sub>

</div>

<br>

| Platforms | Price | Size | Category |
|:---:|:---:|:---:|:---:|
| Mac · iPhone · iPad · Vision Pro | Free | 73 MB on Mac | Games |

## What's New

**Version 0.1.5-beta**

- A new name and an original icon: meet Dashdance.
- **Ready to compete:** the dashboard checks your setup and tells you what costs you milliseconds.
- **MetalFX upscaling** (new): render the game at half the internal resolution and let Apple's MetalFX reconstruct the full picture — Off, Balanced, or Quality, in the dashboard, the in-game menu, or `--upscaler`. Where the GPU budget is tight (iPhone, iPad, 4K), it keeps the image crisp at a fraction of the fill rate.
- Choose your own online input delay, from 1 frame for the lowest latency.
- Lower latency on iPhone and iPad: the display stays at full refresh while you play.
- Connect a Controller walks you through Bluetooth pairing.
- Layouts made for every screen, including iPhone Duo.
- Fixes for stale textures, a freeze when entering full screen, and the installer.

## About

Every other way to play Melee on a Mac runs a GameCube in software, one instruction at a time. Dashdance leaves the console out. The game is translated once, ahead of time, into native code for Apple silicon and drawn with Metal. What's left is Melee, fast, with your Slippi Online account built in.

<div align="center">
<img src="docs/images/store/banner-mac-match.jpg" width="880" alt="Every frame, right on time.">
<br><br>
<img src="docs/images/store/banner-mac-dashboard.jpg" width="880" alt="Ready when you are.">
<br><br>
<img src="docs/images/store/banner-mac-menu.jpg" width="880" alt="Settings, mid-match.">
<br><br>
<img src="docs/images/store/banner-iphone.jpg" width="880" alt="Pick up and play.">
<br><br>
<img src="docs/images/store/banner-ipad.jpg" width="880" alt="Room to breathe.">
<br><br>
<img src="docs/images/store/banner-vision.jpg" width="880" alt="Melee, in your space.">
</div>

### Built to compete

- **Shown on the next refresh.** Each finished frame goes to your display's very next refresh, at whatever rate your display runs.
- **Your inputs, read last.** Controllers are read right before each frame starts.
- **GameCube controllers at 1000 Hz.** Plug in a GameCube adapter for Wii U and Switch. No driver, no security changes.
- **Your delay, your call.** Pick Slippi's online input delay yourself, from 1 frame.
- **Ready to compete.** The dashboard flags a 60 Hz display, Wi-Fi instead of a cable, a slow controller, Low Power Mode and Bluetooth audio.

## Get Dashdance

**You'll need:**

- A Mac with Apple silicon (M1 or newer).
- Your own copy of Super Smash Bros. Melee (NTSC 1.02) as a disc image. Dashdance doesn't include the game.
- A free [Slippi account](https://slippi.gg) to play online.

**Install.** Open Terminal: press ⌘ Space, type *Terminal* and press Return. Paste this line and press Return:

```bash
curl -fsSL https://raw.githubusercontent.com/TheAndersMadsen/dashdance/main/install.sh | zsh
```

When it asks, drag your Melee disc image into the Terminal window and press Return. Dashdance builds itself on your Mac from your own disc, which takes ten to fifteen minutes the first time. Then it's in your Applications folder, and it opens. To update, paste the same line again.

**On iPhone or iPad.** Dashdance can't be on the App Store, because it's built from your own disc. You can still put it on your own device with AltStore, SideStore or Sideloadly. For iPhone Duo, build with Xcode 27.1 or later so it fills both displays. [See how](docs/TECHNICAL.md#iphone-and-ipad-your-own-device).

## Information

| | |
|---|---|
| **Developer** | TheAndersMadsen and contributors |
| **Size** | 73 MB on Mac · 19 MB for iPhone and iPad |
| **Category** | Games |
| **Mac** | Apple silicon, macOS 14 or later (Liquid Glass on macOS 26) |
| **iPhone** | iOS 17 or later |
| **iPad** | iPadOS 17 or later |
| **Apple Vision Pro** | Tested on visionOS 26 in the Simulator |
| **Languages** | English |
| **Price** | Free |
| **License** | GPL-2.0-or-later (GPL-3.0 for builds with the controller artwork) |

## App Privacy

**Dashdance collects no data.** It has no analytics, no tracking and no account of its own. Your disc image stays on your device.

It connects only to what it needs:

- **Slippi Online**, to sign in, load your profile and rank, and find and report matches. Slippi's sign-in runs on Google's Firebase.
- **ipify**, to show the public IP address Slippi's matchmaking sees, on the Matchmaking region card.
- **The Discord app on your Mac**, locally, when Discord presence is on.

## Questions

**Is this official?**
No. Dashdance is an independent community project. It isn't made, endorsed or supported by the Slippi team or Nintendo.

**Can I just download the app?**
No, and there will never be a download. The finished app contains Melee's code, so it has to be built from your own disc, on your own Mac.

**Can I play people who use Slippi Dolphin?**
Yes. Dashdance uses Slippi's own matchmaking and netcode.

**Does it change anything on my Mac?**
It installs Apple's developer tools and a few build tools, then builds the app. It doesn't install drivers or change your Mac's security settings.

**Something isn't working.**
Dashdance is still in alpha. Please [open an issue](https://github.com/TheAndersMadsen/dashdance/issues) and tell us what happened.

## For developers

How it works, the latency work, controller and adapter internals, device builds and building from source are in the [technical guide](docs/TECHNICAL.md). Measurements are in [PERFORMANCE.md](docs/PERFORMANCE.md). Contributors and coding agents should start with [CLAUDE.md](CLAUDE.md).

## Credits and legal

Built on the work of the [Slippi](https://slippi.gg) team, the [Dolphin](https://dolphin-emu.org) project, [SDL](https://libsdl.org), [Aurora](https://github.com/encounter/aurora), the [doldecomp/melee](https://github.com/doldecomp/melee) contributors, and [Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked), the Windows build this project tracks.

The GameCube controller in the controller editor is the indigo controller from [ControllerOverlays](https://github.com/datkat21/ControllerOverlays) by Kat21 (GPL-3.0). The on-screen controller follows [VirtualFriend](https://github.com/agg23/virtualfriend) by Adam Gastineau (MIT). The Dashdance name, icon and mark are original to this project. Slippi is a trademark of its authors; Dashdance is an independent client that connects to Slippi Online and is not affiliated with it. Device frames in the images are drawn for this project. Design references: [dimillian/Skills](https://github.com/dimillian/Skills), Apple's Human Interface Guidelines, including [Designing for iPhone Duo](https://developer.apple.com/design/human-interface-guidelines/designing-for-iphone-duo), and [PwrGit](https://github.com/pwrdrvr/PwrGit/pull/196) for the Icon Composer layout.

**Licence.** Dashdance is distributed under GPL-2.0-or-later, the licence of the project it forks ([Hero88go/melee-unlocked](https://github.com/Hero88go/melee-unlocked)) and of the Dolphin and Slippi Ishiiruka code it ports. Parts under other licences, and the evidence for each, are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Builds that include the controller artwork are covered by GPL-3.0.

**AI-assisted.** Much of Dashdance was written with AI coding assistants. The U.S. Copyright Office concludes that copyright does not extend to purely AI-generated material, but does protect human-authored expression, including human selection, arrangement and modification of AI output. The licence covers every part that is protected. That doesn't make the project public domain: code derived from the GPL projects it builds on stays under the GPL. Details and sources are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#ai-assisted-development).

Super Smash Bros. Melee is the property of Nintendo and HAL Laboratory. This repository contains no game code or data. You must supply your own legally obtained disc image. This is a factual summary, not legal advice.
