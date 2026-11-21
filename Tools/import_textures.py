# Imports the CC0 photo textures (ambientCG, see Tools/textures_src/CREDITS.txt) into /Game/MR/Textures.
# Run (editor closed): UnrealEditor-Cmd.exe <project> -run=pythonscript -script="M:/Tools/import_textures.py"
# Tools/make_materials.bat runs this first, then builds the materials that use the textures.
import os
import unreal

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'textures_src', 'maps')
DEST = '/Game/MR/Textures'
EAL = unreal.EditorAssetLibrary

tasks = []
for f in sorted(os.listdir(SRC)):
    if not f.lower().endswith('.jpg'):
        continue
    name = 'T_' + os.path.splitext(f)[0]
    if EAL.does_asset_exist(DEST + '/' + name):
        continue
    t = unreal.AssetImportTask()
    t.set_editor_property('filename', os.path.join(SRC, f))
    t.set_editor_property('destination_path', DEST)
    t.set_editor_property('destination_name', name)
    t.set_editor_property('automated', True)
    t.set_editor_property('save', False)
    t.set_editor_property('replace_existing', True)
    tasks.append(t)
if tasks:
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

for f in sorted(os.listdir(SRC)):
    if not f.lower().endswith('.jpg'):
        continue
    path = DEST + '/T_' + os.path.splitext(f)[0]
    tex = EAL.load_asset(path)
    if not tex:
        unreal.log_warning('MR: failed to import ' + f)
        continue
    if f.endswith('_NormalDX.jpg'):
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('srgb', False)
    elif f.endswith('_Roughness.jpg'):
        tex.set_editor_property('compression_settings', unreal.TextureCompressionSettings.TC_GRAYSCALE)
        tex.set_editor_property('srgb', False)
    else:
        tex.set_editor_property('srgb', True)
    EAL.save_asset(path)
unreal.log('MR: textures imported')
