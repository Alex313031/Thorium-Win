# Thorium-Win NOTE:INCOMPLETE, mostly just a copy of readme from Thorium, needs updating for windows build flow. Shell scripts need updating for windows as batch files.
Chromium fork for windows named after radioactive element No. 90, windows builds of https://github.com/Alex313031/Thorium

<img src="https://github.com/Alex313031/Thorium/blob/main/logos/product_logo_256.png">

## Chromium fork for windows named after radioactive element No. 90, windows builds of https://github.com/Alex313031/Thorium
- Always built with latest x64 tip-o-tree "Trunk" build of chromium \
- Intended to behave the most like Google Chrome, with differences listed below. \

&nbsp;&nbsp;&ndash; Includes Widevine, FFmpeg, Chrome Plugins, as well as thinLTO and PGO compiler optimizations. It is built with SSE3 and AVX, so it won't launch on CPU's below 2nd gen Core or AMD FX, but benefits from Advanced Vector EXtensions. You can disable this and use regular SSE3 like Chromium and Chrome. (See below.)

### EXPERIMENTAL FEATURES/DIFFERENCES BETWEEN CHROMIUM AND THORIUM
> - Experimental MPEG-DASH.
> - Experimental PDF annotation support (called "Ink" on ChromiumOS).
> - Patches from Debian including font rendering patch, VAAPI Patch, native notifications patch, title bar patch, and... the VDPAU Patch!! (Rejoice Nvidia users)
> - DoH (DNS over HTTPS) patches from Bromite.
> - Enable Do Not Track by default patch from Vanadium.
> - Disable Google API Key warning (you can still use API Keys to enable sync), from Ungoogled Chromium.
> - Includes DuckDuckGo and Ask.com in all locales, along with normal search engines, from Ubuntu.
> - Logo and Branding/Naming changed to Thorium logo, Thorium name, and "Alex313031" being appended to "The Chromium Authors" in credits, etc.
> - Includes installer patches and files to include ChromeDriver and Content-Shell, with a .desktop file being provided for content shell (named Thorium Content Shell.)

&nbsp;&nbsp;&ndash; args.gn exclude API Keys (you can get them yourself) and the pgo profile path is different for each chromium version. (See below.)

&nbsp;&nbsp;In general follow build instructions at https://chromium.googlesource.com/chromium/src/+/refs/heads/main/docs/linux/build_instructions.md and API Keys (if desired) at https://www.chromium.org/developers/how-tos/api-keys

# Building
_**The scripts assume the Chromium source is at $HOME/chromiums/src/. You may have to 'sudo chmod +x' the scripts to make them executable.**_ 
- After initial download of Chromium source code, run (from where you cloned this repo) `./trunk.sh`. This will update and sync the sources and at the end it will download the PGO profile for chromium. The file will be downloaded to *//chromium/src/chrome/build/pgo_profiles/&#42;.profdata* with the actual file name looking something like 'chrome-linux-main-1632505958-ddbb37bcdfa7dbd7b10cf3a9b6a5bc45e7a958a6.profdata', which should be added to the end of args.gn as per below.
- Then (from where you cloned this repo) run `./setup.sh`. This will copy all the files and patches to the needed locations and drop you to *//chromium/src*.
- Run `export EDITOR=nano` & `export VISUAL=nano` *# You can substitute a cmdline editor like vim here, but many GUI editors cause it to try and parse the args.gn file before it is even saved.*
- Run `gn args out/thorium` and the contents of 'args.gn' in this repo should be copy/pasted into the editor. *--Include your api keys here at the top or leave blank, and edit the last line to point to the actual path and file name of '&#42;.profdata'*
- 'args.list' contains an alphabetical list with descriptions of all possible build arguments.
- To build, run `autoninja -j8 -C out/thorium chrome chrome_sandbox content_shell -d stats` *The -j# can be changed to limit or increase the number of jobs (generally should be the number of CPU cores on your machine), and the -d stats at the end just shows better verbose stats during compiling. You could also append chromedriver after content_shell to build chromedriver, the selenium compatible browser fuzzing library.*
- To install, copy/paste the contents of your *out/thorium* dir to a good location i.e. *$HOME/bin/thorium*. **RECOMMENDED - Copy and run clean.sh within this dir to clean up build artifacts**. Then you can just run the browser with `~/bin/thorium/chrome` or the content_shell with `~/bin/thorium/content_shell`.
- **Proper Install:** To install with a .deb, dont copy the contents of *out/thorium*, instead run <br/> `autoninja -C out/thorium/ "chrome/installer/linux:unstable_deb"` A nice .deb file will now be in *out/thorium* and you can install it with `sudo dpkg -i *.deb` It will be called 'thorium-browser-unstable_$VERSIONNUMBER_amd64.deb', and will be installed to */opt/chromium.org/chromium/*. \
&nbsp;&nbsp; NOTE: To get back to "Trunk", i.e. to revert all changes in order to build vanilla chromium, just run `trunk.sh` again. \
&nbsp;&nbsp; NOTE: To compile without AVX, simply go to *//chromium/src/build/config/compiler/BUILD.gn*, search for *mavx* (there's only two <br/> &nbsp;&nbsp; lines), and replace *mavx* with *msse3*.

&minus;Thanks to https://github.com/robrich999/ for some info that went into this project.\
&minus;Also thanks to https://github.com/bromite/bromite, https://github.com/saiarcot895/chromium-ubuntu-build, https://github.com/Eloston/ungoogled-chromium, and https://github.com/GrapheneOS/Vanadium for patch code.

&nbsp;&nbsp; NOTE: libpepflashplayer.so is included for posterity and can be used to enable Adobe Flash on older Chromium releases. ʘ‿ʘ
