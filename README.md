# obs-ntr

## Introduction

The obs-ntr plugin is a plugin for [OBS Studio](https://obsproject.com) which is intended to allow a direct
connection to the [NTR debugger](https://github.com/44670/NTR) running on a Nintendo 3DS for purposes of 
displaying NTR's remote view image stream. EFfectively, this allows you to display your 3DS screen within 
OBS Studio. 

This does require that you have a means of installing homebrew applications on your 3DS, and also only works 
with the "New" 3DS hardware. Explaining how to do this is far beyond the scope of this document; if you do
not already have your 3DS set up to run NTR, please refer to guides like [this](https://3ds.guide/) and 
[this](https://blog.lvlupdojo.com/how-to-stream-from-your-nintendo-3ds-12d7fd115981) which explain the process
far better than I'd be likely to. 

Given a working connection to NTR, this plugin allows you to display the remote view from it without the need
to run NTRViewer or any other external application. It treats the top and bottom screens from the 3DS as
separate sources, meaning they can be positioned and filtered independently if desired. 

## Installation

The binary package mirrors the structure of the OBS Studio installation directory, so you should be able to
just drop its contents alongside an OBS Studio install (usually at C:\Program Files (x86)\obs-studio\). The 
necessary files should look like this: 

    obs-studio
    |---data
    |   |---obs-plugins
    |       |---obs-ntr
    |           |---locale
    |               |---en-US.ini
    |---obs-plugins
        |---32bit
        |   |---obs-ntr.dll
        |   |---turbojpeg.dll
        |---64bit
            |---obs-ntr.dll
            |---turbojpeg.dll

## Usage

The source provided by this plugin is called "3DS capture (NTR)" in OBS Studio's source creation menu. Each
such source represents one of the 3DS's two screens, so in most applications you will probably want to have
at least two of them in your scene. The most obvious property for this source is which screen it will
display: "Bottom" or "Top."

To save system resources, all instances of the obs-ntr source share connection data. Because at this time,
options in OBS cannot be set outside of the context of some particular source, one of the instances must
identify itself as being responsible for the connection. To do this, press the "Claim Responsibility for
NTR Connection" button for the desired source. This should immediately present a number of additional options.

If you know you will be playing games that disable Wi-Fi after lauch (Ocarina of Time, Pokémon) and you are on
firmware 11.4 or later, you can check the option to send a patch to the system prior to connecting. If you do
so, you will want to launch Rosalina with L+Down+Select and go to "Debugger options..." and select
"Enable Debugger". Then back out of the Rosalina menu prior to attempting to connect. The patch will
automatically be sent to the system during the connection process as long as the box is checked and the
debugger has been turned on!

You will need to start by entering the IP address of your 3DS in the "IP Address" box. Unfortunately, finding
a 3DS's IP address is not the easiest thing, and how to do so is beyond the scope of this document. The simplest
way is usually to look at the registered devices in your router's administration interface. 

After entering your IP address, the next thing you will likely need to do is press the "Send NTR Remote View 
Startup Message." This will send a message to the NTR process running on the specified 3DS to begin sending 
remote view packets back to your PC. Note that once NTR starts doing this, there is no way to make it stop
other than to reboot your 3DS (as far as I know). There is also no way to reconfigure it with different settings
without rebooting the 3DS. This step is unnecessary if you have already started up NTR's remote view with another
program like Nitro_Stream. 

The options below that button instruct NTR how it should behave when producing its screen captures. They are the
same options exposed by Nitro_Stream, NTRViewer, and other programs, and should have the same meaning as there.
* "Picture Quality" presumably affects how much it compresses each JPEG frame it sends. 
* I'm not entirely sure at this time what "Quality of Service" does. Apparently any value above 100 disables 
  it, and many sources recommend using a value of 101 to do so. 
* "Priority Factor" and "Priority Screen" seem to affect the ratio of frames sent for each screen. A priority
  factor of 3 with the priority screen set to "Top" would instruct NTR to send 3 times as many frames for the
  top screen as for the bottom screen. I believe a priority of 0 would completely disable one of the screens.
  
Once NTR is sending frames, you can instruct obs-ntr to start receiving them with the "Connect to NTR" button. 
You can stop receiving at any time subsequently if desired by pressing "Disconnect from NTR."

The "Write Connection Stats to Log" option will output statistics about the number of frames obs-ntr has dropped
due to incomplete data. As far as I can tell, these are computed the same way that NTRViewer does, so you should
be able to compare performance between the two programs. 

## Building

If you wish to build the obs-ntr plugin from source, you should just need [CMake](https://cmake.org/), 
the OBS Studio libraries and headers, and the TurboJPEG libraries and headers. 

* [obs-ntr source repository](https://github.com/nleseul/obs-ntr)
* [OBS studio source repository](https://github.com/jp9000/obs-studio)
* [libjpeg-turbo project](https://libjpeg-turbo.org/)

I don't believe that the OBS project provides prebuilt libraries; you're probably going to have the best luck
building your own OBS binaries from the source. Refer to the OBS repository for more information on that.

The installers for libjpeg-turbo provide both headers and binaries, so you should be fine there. 

When building in CMake, you will probably need to set two configuration values so your environment can be
properly set up:

* OBSSourcePath should refer to the libobs subfolder in the OBS source release. The build pipeline will look
  for headers in this location, and for libraries in a "build" folder relative to that path (where the OBS 
  build process puts them). 
* TurboJPEG path should refer to the root folder of a libjpeg-turbo installation. The installer will make this
  C:\libjpeg-turbo\ or C:\libjpeg-turbo64\ by default. This folder should contain include\, lib\, and bin\ 
  subfolders.

The CMake INSTALL script should function properly, as long as you set CMAKE_INSTALL_PREFIX to the root folder
of the OBS Studio installation to which you wish to install. 

## License

This project is licensed under the "[Unlicense](http://unlicense.org/)", because copy[right|left] is a hideous
mess to deal with and I don't like it. 

Unfortunately, this software is based in part on the work of the Independent JPEG Group. I'd love to avoid that
dependency and use the JPEG handling that OBS Studio already provides, but it doesn't seem to be exposed in precisely 
the right way for now. Until then, refer to either the [libjpeg-turbo repository](https://github.com/libjpeg-turbo) or 
the included LICENSE-TurboJPEG.md file for further details. 
