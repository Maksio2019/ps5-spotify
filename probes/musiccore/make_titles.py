# Builds two test titles from the app folder in dist/PPSA99777 whose
# param.json asks the system music core (NPXS40201) to run a web page, like
# the official Spotify app (PPSA05688) does:
#   PPSA99778 - Spotify's own TV web player
#   PPSA99779 - probes/musiccore/page.html served from this PC
# Output: build/musiccore/<TITLE_ID>/
import json, os, shutil, sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
source = os.path.join(root, "dist", "PPSA99777")
pc_ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.90"

titles = {
    "PPSA99778": ("Spotify Core Test",
                  "https://api-partner.spotify.com/tvapp?platform=ps5-cmc&containerVersion=01.012.000"),
    "PPSA99779": ("Music Core Probe", "http://%s:8765/" % pc_ip),
}

for title_id, (name, uri) in titles.items():
    target = os.path.join(root, "build", "musiccore", title_id)
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target)
    path = os.path.join(target, "sce_sys", "param.json")
    param = json.load(open(path, encoding="utf-8-sig"))
    param["titleId"] = title_id
    param["contentId"] = "UP9000-%s_00-MUSICCORETEST000" % title_id
    param["conceptId"] = title_id[4:]
    param["localizedParameters"] = {"defaultLanguage": "en-US", "en-US": {"titleName": name}}
    # The official Spotify app's music-core fields.
    param["applicationCategoryType"] = 66048
    param["attribute"] = 1644167168
    param["musicCoreName"] = "CustomMusicCore"
    param["musicCoreTitleId"] = "NPXS40201"
    param["webAppUri"] = uri
    param["displayLocation"] = 188
    param["serviceLaunchButtonKeyCode"] = 2
    json.dump(param, open(path, "w"), indent=2)
    print(title_id, "->", target)
