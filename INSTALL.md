# Installing MCLCE-Android

## The app

- Either copy the .apk to your device, or enable 'USB debugging' on your phone and install it from a computer using 'adb install app-release.apk'

If you open the .apk on the device itself, allow the install when prompted.

## Getting the game files

There are no game files included, you'll need to obtain them from a working copy of the TU19 Windows64 build

- After installing the application, launch it once and it will create a directory: 'Android/data/com.legacy.mclce/files'. Copy the 'Common', 'Windows64Media' and 'music' folders from your Windows64 copy into this directory, ensuring correct folder and file names are used
- Quit the application, and restart it. The initial load will take longer as it unpacks 'MediaWindows64.arc'

On Android 11 and later, you won't be able to browse the 'Android/data' directory using the phone's file manager. You'll need to copy the files from your computer via USB, or use 'adb':

```
adb push Common /sdcard/Android/data/com.legacy.mclce/files/
adb push Windows64Media /sdcard/Android/data/com.legacy.mclce/files/
adb push music /sdcard/Android/data/com.legacy.mclce/files/
```

Skin/texture packs should be placed in 'Windows64Media/DLC', in the same manner as for Windows64. Other data is stored in a 'saves' directory and 'settings.dat' created by the application.

## Common problems

- 'App not installed': there may be an earlier version of the application installed, using a different key, which must be removed (this will lose any associated saves, so back them up first)
- Controller won't work: ensure it's paired via the phone's bluetooth settings, or connected using a USB OTG adaptor before starting the application
- Incorrect/blank menus: 'Common/Media/MediaWindows64.arc' may be missing, or the application wasn't correctly quit and restarted after files were copied
