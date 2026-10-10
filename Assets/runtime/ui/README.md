# Portable runtime UI atlases

These unmodified copies keep the CMake/Ninja dependency graph free of non-ASCII paths on English Windows runners. Original artwork remains in `Assets/未处理UI/`; do not rename or delete that source directory.

| Runtime file | Original basename | SHA256 |
|---|---|---|
| notepad_atlas.png | sactx-0-512x256-DXT5_BC3-NotepadAtlas-a8efa592.png | 4f702c3608ef36e4c7f0cce7b4ab024979f4a14011f738158eb5c1c40586eb28 |
| statusbar_atlas.png | sactx-0-2048x1024-DXT5_BC3-StatusbarAtlas-be4c2c94.png | 11d54f479372cfaa582073a26720d60df2adef15360458595b995b7ab8e3873c |

`broadcast_assets.qrc` references only these ASCII paths while preserving its `:/broadcast/...` aliases. Visual output is unchanged. The HUD composition tool continues to read the original source artwork.
