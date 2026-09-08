import discord
from discord.ext import tasks
import json
import os
import urllib.request
import urllib.parse
import re
import time

TOKEN = ''
STEAM_API_KEY = 'DC13DD1969A48C1F18DC43129B206F67'
CHANNEL_NAME = 'steam-player-tracker'
CHANNEL_ID = 1543627990380978244

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JSON_PATH = os.path.normpath(os.path.join(SCRIPT_DIR, 'tracker_config.json'))
KNOWN_BANS_FILE = os.path.join(SCRIPT_DIR, 'known_bans.json')

intents = discord.Intents.default()
intents.guilds = True
intents.messages = True
intents.message_content = True  
bot = discord.Client(intents=intents)

def load_json_data():
    if os.path.exists(JSON_PATH):
        try:
            with open(JSON_PATH, 'r', encoding='utf-8') as f:
                return json.load(f)
        except Exception:
            return {"players": []}
    return {"players": []}

def save_json_data(data):
    with open(JSON_PATH, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=4)

def load_known_bans():
    if os.path.exists(KNOWN_BANS_FILE):
        try:
            with open(KNOWN_BANS_FILE, 'r') as f:
                bans = json.load(f)
                print(f"[Startup] Loaded {len(bans)} known bans from disk.")
                return bans
        except Exception as e:
            print(f"[Error] Could not read known_bans.json: {e}")
            return []
    print(f"[Startup] known_bans.json does not exist yet. Starting fresh.")
    return []

def save_known_bans(known_bans_list):
    try:
        with open(KNOWN_BANS_FILE, 'w') as f:
            json.dump(known_bans_list, f, indent=4)
        print(f"[Debug] Successfully saved {len(known_bans_list)} known bans to disk.")
    except Exception as e:
        print(f"[Error] FAILED to save known_bans.json: {e}")

known_bans = load_known_bans()

async def find_channel():
    if CHANNEL_ID and CHANNEL_ID != 0:
        ch = bot.get_channel(CHANNEL_ID)
        if ch:
            return ch
        try:
            return await bot.fetch_channel(CHANNEL_ID)
        except Exception:
            pass

    for guild in bot.guilds:
        for channel in guild.text_channels:
            if channel.name.lower() == CHANNEL_NAME.lower():
                return channel
    return None

def resolve_steam_identifier(input_str, api_key):
    input_str = input_str.strip()
    print(f"[Debug] Parsing input: {input_str}")
    
    # 1. Check if it's already a 17-digit SteamID64
    if input_str.isdigit() and len(input_str) == 17:
        print(f"[Debug] Direct SteamID64 detected: {input_str}")
        return input_str
        
    # 2. Check if it's a full URL containing /profiles/
    profiles_match = re.search(r'steamcommunity\.com/profiles/(\d{17})', input_str)
    if profiles_match:
        steam_id = profiles_match.group(1)
        print(f"[Debug] Extracted SteamID64 from profile URL: {steam_id}")
        return steam_id
        
    # 3. Check if it's a custom vanity URL containing /id/
    vanity_match = re.search(r'steamcommunity\.com/id/([^/?#]+)', input_str)
    if vanity_match:
        vanity_name = vanity_match.group(1)
        print(f"[Debug] Extracted vanity name from URL: {vanity_name}")
        return resolve_vanity_name(vanity_name, api_key)
        
    # 4. If it's a plain string without slashes/dots, try treating it as a raw vanity name
    if not ("/" in input_str or "." in input_str):
        print(f"[Debug] Treated as raw vanity name: {input_str}")
        return resolve_vanity_name(input_str, api_key)
        
    print(f"[Debug] Failed to parse identifier from: {input_str}")
    return None

def resolve_vanity_name(vanity_name, api_key):
    try:
        resolve_url = f"https://api.steampowered.com/ISteamUser/ResolveVanityURL/v0001/?key={api_key}&vanityurl={vanity_name}"
        with urllib.request.urlopen(resolve_url) as response:
            data = json.loads(response.read().decode())
            response_data = data.get("response", {})
            if response_data.get("success") == 1:
                resolved_id = response_data.get("steamid")
                print(f"[Debug] Successfully resolved vanity '{vanity_name}' to ID64: {resolved_id}")
                return resolved_id
            else:
                print(f"[Error] Steam API could not resolve vanity name '{vanity_name}'.")
    except Exception as e:
        print(f"[Error] Failed to resolve vanity URL '{vanity_name}': {e}")
    return None

@bot.event
async def on_ready():
    print(f"==================================================")
    print(f"Logged in as: {bot.user}")
    print(f"Monitoring config at: {JSON_PATH}")
    print(f"==================================================")
    if not check_bans.is_running():
        check_bans.start()

@bot.event
async def on_message(message):
    if message.author == bot.user:
        return

    if message.content.startswith('!track '):
        parts = message.content.split(' ')
        if len(parts) < 2:
            await message.channel.send("❌ Please provide a SteamID64 or Steam Profile URL. Example: `!track https://steamcommunity.com/id/dieselv2`")
            return

        raw_input = parts[1].strip()
        steam_id = resolve_steam_identifier(raw_input, STEAM_API_KEY)

        if not steam_id or not steam_id.isdigit() or len(steam_id) != 17:
            await message.channel.send("❌ Could not resolve a valid 17-digit SteamID64 from that input. Please check the link or ID and try again.")
            return

        data = load_json_data()
        players = data.get('players', [])

        for p in players:
            existing_id = str(p.get('id') or p)
            if existing_id == steam_id:
                await message.channel.send(f"⚠️ Player `{steam_id}` is already tracked!")
                return

        # Fetch live data from Steam Web API immediately
        name = "Unknown"
        avatar_url = ""
        level = -1
        age = -1
        cs2_hours = -1
        is_banned = False
        vac_bans = 0
        game_bans = 0

        try:
            summary_url = f"https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v0002/?key={STEAM_API_KEY}&steamids={steam_id}"
            with urllib.request.urlopen(summary_url) as response:
                summary_data = json.loads(response.read().decode())
                players_list = summary_data.get("response", {}).get("players", [])
                if players_list:
                    p_info = players_list[0]
                    name = p_info.get("personaname", "Unknown")
                    avatar_url = p_info.get("avatarfull", "")
                    time_created = p_info.get("timecreated", 0)
                    if time_created > 0:
                        now = time.time()
                        age = int((now - time_created) / (365.25 * 24 * 3600))

            level_url = f"https://api.steampowered.com/IPlayerService/GetSteamLevel/v1/?key={STEAM_API_KEY}&steamid={steam_id}"
            try:
                with urllib.request.urlopen(level_url) as response:
                    level_data = json.loads(response.read().decode())
                    level = level_data.get("response", {}).get("player_level", -1)
            except Exception:
                pass

            games_url = f"https://api.steampowered.com/IPlayerService/GetOwnedGames/v0001/?key={STEAM_API_KEY}&steamid={steam_id}&appids_filter[0]=730"
            try:
                with urllib.request.urlopen(games_url) as response:
                    games_data = json.loads(response.read().decode())
                    owned_games = games_data.get("response", {}).get("games", [])
                    if owned_games:
                        cs2_hours = owned_games[0].get("playtime_forever", 0) // 60
            except Exception:
                pass

            bans_url = f"https://api.steampowered.com/ISteamUser/GetPlayerBans/v1/?key={STEAM_API_KEY}&steamids={steam_id}"
            try:
                with urllib.request.urlopen(bans_url) as response:
                    bans_data = json.loads(response.read().decode())
                    bans_list = bans_data.get("players", [])
                    if bans_list:
                        b_info = bans_list[0]
                        vac_banned = b_info.get("VACBanned", False)
                        vac_bans = b_info.get("NumberOfVACBans", 0)
                        game_bans = b_info.get("NumberOfGameBans", 0)
                        community_banned = b_info.get("CommunityBanned", False)
                        is_banned = vac_banned or (game_bans > 0) or community_banned
            except Exception:
                pass

        except Exception as e:
            print(f"[Error] Failed fetching Steam API data during !track: {e}")

        new_player_entry = {
            "id": steam_id,
            "name": name,
            "avatarUrl": avatar_url,
            "level": level,
            "age": age,
            "cs2_hours": cs2_hours,
            "banned": is_banned,
            "vacBans": vac_bans,
            "gameBans": game_bans,
            "note": "Added via Discord bot"
        }
        
        players.append(new_player_entry)
        data['players'] = players
        save_json_data(data)

        await message.channel.send(f"✅ Added SteamID `{steam_id}` ({name}) to tracker configuration with up-to-date stats!")

@tasks.loop(seconds=30)
async def check_bans():
    target_channel = await find_channel()
    if not target_channel:
        return

    data = load_json_data()
    players = data.get('players', [])

    for player in players:
        steam_id = str(player.get('id', ''))
        is_banned = player.get('banned', False)

        if not steam_id.isdigit() or len(steam_id) != 17:
            continue

        if is_banned:
            if steam_id in known_bans:
                continue

            known_bans.append(steam_id)
            save_known_bans(known_bans)

            name = player.get('name', 'Unknown')
            avatar_url = player.get('avatarUrl', '')
            steam_url = f"https://steamcommunity.com/profiles/{steam_id}"
            
            print(f"[Alert] New ban detected for {name} ({steam_id})! Sending Discord message...")

            embed = discord.Embed(
                title="BANNED",
                description=f"**[{name}]({steam_url})** just received a ban!",
                color=0xED4245
            )

            if avatar_url:
                embed.set_thumbnail(url=avatar_url)

            level = player.get('level', -1)
            hours = player.get('cs2_hours', -1)
            age = player.get('age', -1)
            vac_bans = player.get('vacBans', 0)
            game_bans = player.get('gameBans', 0)
            note = player.get('note', '')

            embed.add_field(name="SteamID", value=f"`{steam_id}`", inline=False)
            embed.add_field(name="Steam Level", value=str(level) if level >= 0 else "Private", inline=True)
            embed.add_field(name="CS2 Hours", value=f"{hours}h" if hours >= 0 else "Private", inline=True)
            embed.add_field(name="Account Age", value=f"{age} Yrs" if age >= 0 else "Private", inline=True)
            embed.add_field(name="Ban Record", value=f"VAC: **{vac_bans}** | Game: **{game_bans}**", inline=False)

            if note:
                embed.add_field(name="Custom Note", value=f"*{note}*", inline=False)

            try:
                await target_channel.send(embed=embed)
                print(f"[Alert] Message sent successfully to #{target_channel.name}!")
            except discord.Forbidden:
                print(f"[Error] Missing permissions in #{target_channel.name}!")
            except Exception as ex:
                print(f"[Error] Failed to send message: {ex}")

bot.run(TOKEN)
