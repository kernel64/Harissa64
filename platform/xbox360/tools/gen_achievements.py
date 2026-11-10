#!/usr/bin/env python3
"""Harissa64's Xbox 360 achievements: the list, its texts, icons and ids.

Writes, from the table below:
  title/harissa64v2.gameconfig   the title data spac compiles into the XEX
                                 (name, title image, achievements; en-US and fr-FR)
  title/art/ach/NN.png           one 64x64 icon per achievement
  xb_ach_ids.h                   XB_ACH_* ids for the code

The ids and the gamerscore are part of what players' profiles keep: once a
release holds this list, never renumber, remove or re-score an entry; new
ones go at the end.

Usage: python platform/xbox360/tools/gen_achievements.py [--check]
"""
import os, sys
from xml.sax.saxutils import escape

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
XB = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(XB))
FONT = os.path.join(ROOT, "third_party", "inter", "Inter-SemiBold.ttf")

RED, GOLD, BLUE, PURPLE, GREEN, GREY = (214, 58, 44), (224, 160, 30), (46, 120, 210), (136, 80, 200), (40, 150, 90), (90, 98, 116)

# id, key, gamerscore, colour, icon text, (title, how to get it, once you have it) in English then French.
# RA = RetroAchievements.
A = [
    (1, "FIRST", 10, RED, "1",
     ("First Step", "Unlock a RetroAchievements achievement in any game.", "Unlocked a first RetroAchievements achievement."),
     ("Premier pas", "Débloquez un succès RetroAchievements dans un jeu.", "Premier succès RetroAchievements débloqué.")),
    (2, "COLLECT_10", 20, RED, "10",
     ("Collector I", "Unlock 10 RetroAchievements achievements.", "Unlocked 10 RetroAchievements achievements."),
     ("Collectionneur I", "Débloquez 10 succès RetroAchievements.", "10 succès RetroAchievements débloqués.")),
    (3, "COLLECT_50", 40, RED, "50",
     ("Collector II", "Unlock 50 RetroAchievements achievements.", "Unlocked 50 RetroAchievements achievements."),
     ("Collectionneur II", "Débloquez 50 succès RetroAchievements.", "50 succès RetroAchievements débloqués.")),
    (4, "COLLECT_100", 60, RED, "100",
     ("Collector III", "Unlock 100 RetroAchievements achievements.", "Unlocked 100 RetroAchievements achievements."),
     ("Collectionneur III", "Débloquez 100 succès RetroAchievements.", "100 succès RetroAchievements débloqués.")),
    (5, "COLLECT_250", 80, RED, "250",
     ("Collector IV", "Unlock 250 RetroAchievements achievements.", "Unlocked 250 RetroAchievements achievements."),
     ("Collectionneur IV", "Débloquez 250 succès RetroAchievements.", "250 succès RetroAchievements débloqués.")),
    (6, "COLLECT_500", 100, RED, "500",
     ("Collector V", "Unlock 500 RetroAchievements achievements.", "Unlocked 500 RetroAchievements achievements."),
     ("Collectionneur V", "Débloquez 500 succès RetroAchievements.", "500 succès RetroAchievements débloqués.")),
    (7, "POINTS_100", 20, GOLD, "100",
     ("Scorer I", "Earn 100 RetroAchievements points.", "Earned 100 RetroAchievements points."),
     ("Marqueur I", "Gagnez 100 points RetroAchievements.", "100 points RetroAchievements gagnés.")),
    (8, "POINTS_1000", 50, GOLD, "1K",
     ("Scorer II", "Earn 1,000 RetroAchievements points.", "Earned 1,000 RetroAchievements points."),
     ("Marqueur II", "Gagnez 1 000 points RetroAchievements.", "1 000 points RetroAchievements gagnés.")),
    (9, "POINTS_5000", 100, GOLD, "5K",
     ("Scorer III", "Earn 5,000 RetroAchievements points.", "Earned 5,000 RetroAchievements points."),
     ("Marqueur III", "Gagnez 5 000 points RetroAchievements.", "5 000 points RetroAchievements gagnés.")),
    (10, "GAMES_3", 20, BLUE, "3",
     ("Explorer I", "Unlock achievements in 3 different games.", "Unlocked achievements in 3 different games."),
     ("Explorateur I", "Débloquez des succès dans 3 jeux différents.", "Succès débloqués dans 3 jeux différents.")),
    (11, "GAMES_10", 50, BLUE, "10",
     ("Explorer II", "Unlock achievements in 10 different games.", "Unlocked achievements in 10 different games."),
     ("Explorateur II", "Débloquez des succès dans 10 jeux différents.", "Succès débloqués dans 10 jeux différents.")),
    (12, "GAMES_25", 80, BLUE, "25",
     ("Explorer III", "Unlock achievements in 25 different games.", "Unlocked achievements in 25 different games."),
     ("Explorateur III", "Débloquez des succès dans 25 jeux différents.", "Succès débloqués dans 25 jeux différents.")),
    (13, "MASTER_1", 80, PURPLE, "100%",
     ("Perfectionist I", "Unlock every achievement of a game.", "Unlocked every achievement of a game."),
     ("Perfectionniste I", "Débloquez tous les succès d'un jeu.", "Tous les succès d'un jeu débloqués.")),
    (14, "MASTER_3", 100, PURPLE, "x3",
     ("Perfectionist II", "Unlock every achievement of 3 games.", "Unlocked every achievement of 3 games."),
     ("Perfectionniste II", "Débloquez tous les succès de 3 jeux.", "Tous les succès de 3 jeux débloqués.")),
    (15, "SM64", 30, GREEN, "SM64",
     ("Super Mario 64", "Unlock 10 achievements in Super Mario 64.", "Unlocked 10 achievements in Super Mario 64."),
     ("Super Mario 64", "Débloquez 10 succès dans Super Mario 64.", "10 succès débloqués dans Super Mario 64.")),
    (16, "OOT", 30, GREEN, "OoT",
     ("Ocarina of Time", "Unlock 10 achievements in The Legend of Zelda: Ocarina of Time.", "Unlocked 10 achievements in Ocarina of Time."),
     ("Ocarina of Time", "Débloquez 10 succès dans The Legend of Zelda: Ocarina of Time.", "10 succès débloqués dans Ocarina of Time.")),
    (17, "MK64", 30, GREEN, "MK64",
     ("Mario Kart 64", "Unlock 10 achievements in Mario Kart 64.", "Unlocked 10 achievements in Mario Kart 64."),
     ("Mario Kart 64", "Débloquez 10 succès dans Mario Kart 64.", "10 succès débloqués dans Mario Kart 64.")),
    (18, "GE007", 30, GREEN, "007",
     ("GoldenEye 007", "Unlock 10 achievements in GoldenEye 007.", "Unlocked 10 achievements in GoldenEye 007."),
     ("GoldenEye 007", "Débloquez 10 succès dans GoldenEye 007.", "10 succès débloqués dans GoldenEye 007.")),
    (19, "BANJO", 30, GREEN, "BK",
     ("Banjo-Kazooie", "Unlock 10 achievements in Banjo-Kazooie.", "Unlocked 10 achievements in Banjo-Kazooie."),
     ("Banjo-Kazooie", "Débloquez 10 succès dans Banjo-Kazooie.", "10 succès débloqués dans Banjo-Kazooie.")),
    (20, "SAVE_STATE", 10, GREY, "SAVE",
     ("Quick Save", "Save a state in a game.", "Saved a state in a game."),
     ("Sauvegarde éclair", "Sauvegardez un état dans un jeu.", "Un état sauvegardé dans un jeu.")),
    (21, "HD_PACK", 10, GREY, "HD",
     ("High Definition", "Install an HD texture pack.", "Installed an HD texture pack."),
     ("Haute définition", "Installez un pack de textures HD.", "Un pack de textures HD installé.")),
    (22, "FAVOURITE", 10, GREY, "FAV",
     ("My Favourites", "Add a game to your favourites.", "Added a game to your favourites."),
     ("Mes préférés", "Ajoutez un jeu à vos favoris.", "Un jeu ajouté à vos favoris.")),
    (23, "LANGUAGE", 10, GREY, "Aa",
     ("Polyglot", "Change the language of the menus.", "Changed the language of the menus."),
     ("Polyglotte", "Changez la langue des menus.", "Langue des menus changée.")),
]
TOTAL = 1000


def icon(colour, text, path):
    """A 64x64 rounded tile in the category's colour with a short white text."""
    S = 4
    im = Image.new("RGBA", (64 * S, 64 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top = tuple(min(255, int(c * 1.12)) for c in colour)
    bot = tuple(int(c * 0.62) for c in colour)
    grad = Image.new("RGBA", (1, 64 * S))
    for y in range(64 * S):
        k = y / (64 * S - 1)
        grad.putpixel((0, y), tuple(int(top[i] + (bot[i] - top[i]) * k) for i in range(3)) + (255,))
    grad = grad.resize((64 * S, 64 * S))
    mask = Image.new("L", (64 * S, 64 * S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 64 * S - 1, 64 * S - 1), radius=12 * S, fill=255)
    im.paste(grad, (0, 0), mask)
    d.rounded_rectangle((2 * S, 2 * S, 62 * S - 1, 62 * S - 1), radius=10 * S, outline=(255, 255, 255, 70), width=S)
    size = 34 * S
    while True:
        f = ImageFont.truetype(FONT, size)
        box = d.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= 50 * S or size <= 8 * S:
            break
        size -= S
    w, h = box[2] - box[0], box[3] - box[1]
    x, y = (64 * S - w) / 2 - box[0], (64 * S - h) / 2 - box[1]
    d.text((x + S, y + 2 * S), text, font=f, fill=(0, 0, 0, 110))
    d.text((x, y), text, font=f, fill=(255, 255, 255, 255))
    im.resize((64, 64), Image.LANCZOS).save(path)


def gameconfig():
    L = []
    w = L.append
    w('<?xml version="1.0" encoding="utf-8" standalone="no"?>')
    w('<!-- Harissa64 V2 title data, compiled by the XDK\'s spac into spa.bin and embedded in the XEX')
    w('     as the section named after the title ID (48413632, see harissa64v2.vcxproj AdditionalSections).')
    w('     Dashboards (Aurora, the stock dashboard) read the title name (X_STRINGID_TITLENAME), the')
    w('     title image (X_IMAGEID_GAME, 64x64 PNG, made by tools/gen_title_art.py) and the achievements')
    w('     from it. Written by tools/gen_achievements.py: edit the table there, not this file. -->')
    w('<XboxLiveSubmissionProject Version="2.0.60117.01" xmlns="http://www.xboxlive.com/xlast">')
    w('  <GameConfigProject name="Harissa64" titleName="Harissa64" projectVersion="2.00.0000.0" schemaVersion="1.08.0000.0" '
      'classification="BaseGame" titleId="0x48413632" multiplayer="false" titleType="1" arn="0x0">')
    w('    <ProductInformation>')
    w('      <Platform name="Xbox 360" id="2" />')
    w('    </ProductInformation>')
    w('    <Privileges />')
    w('    <LocalizedStrings defaultLocale="en-US" nextId="400">')
    w('      <SupportedLocale locale="en-US" />')
    w('      <SupportedLocale locale="fr-FR" />')

    def s(sid, name, en, fr=None, presence=False):
        w('      <LocalizedString id="%d" friendlyName="%s">' % (sid, name))
        w('        <Translation locale="en-US">%s</Translation>' % escape(en))
        w('        <Translation locale="fr-FR">%s</Translation>' % escape(fr if fr is not None else en))
        if presence:
            w('        <Presence />')
        w('      </LocalizedString>')

    s(32768, "X_STRINGID_TITLENAME", "Harissa64")
    s(1, "STR_PLAYING", "Playing", "En jeu", presence=True)
    s(32512, "X_STRINGID_GAMETYPE", "Game Type", "Type de partie")
    s(32513, "X_STRINGID_RANKED", "Ranked Match", "Partie classée")
    s(32514, "X_STRINGID_STANDARD", "Player Match", "Partie libre")
    for aid, key, cred, col, txt, en, fr in A:
        s(100 + aid, "ACH_%s_NAME" % key, en[0], fr[0])
        s(200 + aid, "ACH_%s_DESC" % key, en[2], fr[2])
        s(300 + aid, "ACH_%s_HOWTO" % key, en[1], fr[1])
    w('    </LocalizedStrings>')
    w('    <Presence>')
    w('      <PresenceMode contextValue="0" stringId="1" friendlyName="STR_PLAYING" />')
    w('    </Presence>')
    w('    <Properties nextId="1" />')
    w('    <Contexts nextId="0x00000000">')
    w('      <Context stringId="32512" friendlyName="GAME_TYPE" id="0x0000800A" defaultValue="0">')
    w('        <ContextValue stringId="32513" value="0" friendlyName="RANKED" />')
    w('        <ContextValue stringId="32514" value="1" friendlyName="STANDARD" />')
    w('      </Context>')
    w('    </Contexts>')
    w('    <Achievements nextId="%d">' % (max(a[0] for a in A) + 1))
    for aid, key, cred, col, txt, en, fr in A:
        w('      <Achievement id="%d" friendlyName="%s" titleStringId="%d" descriptionStringId="%d" unachievedStringId="%d" '
          'showUnachieved="true" achievementType="Other" cred="%d" imageId="%d" />' % (aid, key, 100 + aid, 200 + aid, 300 + aid, cred, aid))
    w('    </Achievements>')
    w('    <Images>')
    for aid, key, cred, col, txt, en, fr in A:
        w('      <Image id="%d" friendlyName="IMG_%s">' % (aid, key))
        w('        <Path>art\\ach\\%02d.png</Path>' % aid)
        w('      </Image>')
    w('      <Image id="32768" friendlyName="X_IMAGEID_GAME">')
    w('        <Path>art\\icon.png</Path>')
    w('      </Image>')
    w('    </Images>')
    w('    <GameModes defaultValue="0" />')
    w('    <StatsViewMasters />')
    w('    <StatsViews nextViewId="1" />')
    w('    <GamerPictures />')
    w('  </GameConfigProject>')
    w('</XboxLiveSubmissionProject>')
    return "\r\n".join(L) + "\r\n"


def header():
    L = ["// Generated by tools/gen_achievements.py - do not edit.",
         "// The ids of Harissa64's Xbox 360 achievements (title/harissa64v2.gameconfig).",
         "#ifndef XB_ACH_IDS_H", "#define XB_ACH_IDS_H", ""]
    for aid, key, cred, col, txt, en, fr in A:
        L.append("#define XB_ACH_%-12s %2d   // %s (%d G)" % (key, aid, en[0], cred))
    L += ["#define XB_ACH_COUNT %d" % len(A), "", "#endif", ""]
    return "\n".join(L)


def main():
    check = "--check" in sys.argv
    total = sum(a[2] for a in A)
    if total != TOTAL:
        sys.exit("ERROR: the gamerscore adds up to %d, not %d" % (total, TOTAL))
    if sorted(a[0] for a in A) != list(range(1, len(A) + 1)):
        sys.exit("ERROR: ids must be 1..N without holes")
    out = {os.path.join(XB, "title", "harissa64v2.gameconfig"): gameconfig().encode("utf-8"),
           os.path.join(XB, "xb_ach_ids.h"): header().encode("utf-8")}
    for path, data in out.items():
        old = open(path, "rb").read() if os.path.isfile(path) else None
        if old != data:
            if check:
                sys.exit("ERROR: %s is not up to date: run tools/gen_achievements.py" % path)
            open(path, "wb").write(data)
    adir = os.path.join(XB, "title", "art", "ach")
    if not check:
        os.makedirs(adir, exist_ok=True)
        for aid, key, cred, col, txt, en, fr in A:
            icon(col, txt, os.path.join(adir, "%02d.png" % aid))
    print("achievements OK: %d achievements, %d G" % (len(A), total))


if __name__ == "__main__":
    main()
