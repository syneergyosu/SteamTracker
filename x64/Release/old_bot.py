import discord
from discord.ext import tasks
import json
import os

TOKEN = ''
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
            await message.channel.send("❌ Please provide a SteamID64. Example: `!track 76561198000000000`")
            return

        steam_id = parts[1].strip()
        if not steam_id.isdigit() or len(steam_id) != 17:
            await message.channel.send("❌ Invalid SteamID64 format. It should be a 17-digit number.")
            return

        data = load_json_data()
        players = data.get('players', [])

        for p in players:
            existing_id = str(p.get('id') or p)
            if existing_id == steam_id:
                await message.channel.send(f"⚠️ Player `{steam_id}` is already tracked!")
                return

        new_player_entry = {
            "id": steam_id,
            "name": "Loading from C++...",
            "level": -1,
            "age": -1,
            "cs2_hours": -1,
            "banned": False,
            "vacBans": 0,
            "gameBans": 0,
            "note": "Added via Discord bot"
        }
        
        players.append(new_player_entry)
        data['players'] = players
        save_json_data(data)

        await message.channel.send(f"✅ Added SteamID `{steam_id}` to tracker configuration!")

@tasks.loop(seconds=1)
async def check_bans():
    target_channel = await find_channel()
    if not target_channel:
        return

    data = load_json_data()
    players = data.get('players', [])

    for player in players:
        steam_id = str(player.get('id', ''))
        is_banned = player.get('banned', False)

        if not steam_id:
            continue

        if is_banned:
            if steam_id in known_bans:
                continue

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
                
                if steam_id not in known_bans:
                    known_bans.append(steam_id)
                    save_known_bans(known_bans)
            except discord.Forbidden:
                print(f"[Error] Missing permissions in #{target_channel.name}!")
            except Exception as ex:
                print(f"[Error] Failed to send message: {ex}")

bot.run(TOKEN)
