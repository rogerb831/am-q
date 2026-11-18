# Documentation tools

## Regenerating the user guide's screenshots

The screenshots in `docs/manual/images` come from the real editor, driven by the GUI screenshot
tool. After a build of the CLAP plugin:

```
build/tests/Release/amq_plugin_gui_shot.exe build/plugin/CLAP/Release/Am-Q.clap <absolute path to docs/manual/images> --manual
python tools/docs/compress_png.py docs/manual/images/*.png
```

Delete `shot-request.txt` from the folder afterwards. The first command saves one PNG per view
(`tests/plugin/plugin_gui_shot.cpp`, the `--manual` sequence) at 1280 x 720; the second rewrites
them as compressed PNGs (about 100 KB each instead of 3.6 MB) with the same pixels. Run it on a
local display at 100 % Windows scaling, so the images are 1280 x 720.
