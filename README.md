# SteamTracker

A lightweight monitoring tool and database designed to log suspicious Steam profiles and automatically track their ban status via a Discord bot.

---

## Features

- **ImGui Desktop GUI:** View, manage, and visualize your tracked profiles directly from `tracker_config.json`.
- **Automated Ban Checks:** A Discord bot service that periodically queries the Steam Web API to verify VAC, Game, and Community ban statuses.
- **Configurable Alerts:** Sends immediate notifications to Discord when a tracked player receives a ban.

---

## Project Structure

```text
├── Build/
│   └── x64/
│       └── Release/
│           └── SteamTracker.exe   # ImGui Desktop GUI
├── bot.py                         # Discord ban tracking service
├── tracker_config.json            # Tracked profiles & local database
├── api_key.txt                    # Steam API key storage
└── main.cpp                       # GUI source code

```
In development and anything can be changed.
