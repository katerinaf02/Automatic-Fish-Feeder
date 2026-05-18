# ----------------- Streamlit + FastAPI -----------------
from fastapi import FastAPI
from pydantic import BaseModel
import threading
import uvicorn
import streamlit as st
from streamlit_autorefresh import st_autorefresh
import pandas as pd
import time
import matplotlib.pyplot as plt
import sqlite3
import math
import requests
import altair as alt


# ---------- OAUTH CONFIG (RESTRICT ACCESS) ----------
ALLOWED_EMAILS = []

def check_google_auth():
    if not hasattr(st.user, "is_logged_in") or not st.user.is_logged_in:
        st.title("🔒 Aquarium Feeder - Private Dashboard")
        st.warning("This dashboard is restricted. Please sign in with authorized Google account.")
        if st.button("Log in with Google"):
            st.login()
        st.stop()
    if st.user.email not in ALLOWED_EMAILS:
        st.error(f"Access Denied: {st.user.email} is not authorized.")
        if st.button("Log out"):
            st.logout()
        st.stop()

check_google_auth()


DISCORD_WEBHOOK_URL = ""


# ---------- DATABASE ----------
DB_FILE = "sensor_data.db"
SECONDS_1_YEAR = 365 * 24 * 60 * 60

def init_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("""
        CREATE TABLE IF NOT EXISTS sensor_data (
            time REAL,
            tempC REAL,
            water REAL,
            tdsVal REAL,
            tdsPpm REAL
       )
    """)
    c.execute("""
        CREATE TABLE IF NOT EXISTS measurement_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            enabled INTEGER
        )
    """)
    c.execute("INSERT OR IGNORE INTO measurement_state (id, enabled) VALUES (1, 1)")

    c.execute("""
        CREATE TABLE IF NOT EXISTS feeder_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            segments INTEGER
        )
    """)
    c.execute("INSERT OR IGNORE INTO feeder_state (id, segments) VALUES (1, 0)")
    c.execute("""
        CREATE TABLE IF NOT EXISTS feed_commands (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            pending INTEGER
        )
    """)
    c.execute("INSERT OR IGNORE INTO feed_commands (id, pending) VALUES (1, 0)")
    try:
        c.execute("ALTER TABLE feed_commands ADD COLUMN command_token INTEGER DEFAULT 0")
    except:
        pass
    try:
        c.execute("ALTER TABLE feed_commands ADD COLUMN expires_at REAL DEFAULT 0")
    except:
        pass
    try:
        c.execute("ALTER TABLE feed_commands ADD COLUMN acknowledged_token INTEGER DEFAULT 0")
    except:
        pass
    c.execute("""
        CREATE TABLE IF NOT EXISTS water_settings (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            max_level REAL,
            min_distance REAL
        )
    """)
    c.execute("INSERT OR IGNORE INTO water_settings (id, max_level, min_distance) VALUES (1, NULL, 0)")
    
    # Add temperature columns if they don't exist
    try:
        c.execute("ALTER TABLE water_settings ADD COLUMN max_temp REAL DEFAULT 30.0")
    except:
        pass
    try:
        c.execute("ALTER TABLE water_settings ADD COLUMN min_temp REAL DEFAULT 24.0")
    except:
        pass

    conn.commit()
    conn.close()

init_db()

# ---------- CAMERA ----------
CAMERA_URL = "http://192.168.1.50/stream"

# ---------- FASTAPI ----------
app = FastAPI()

class SensorData(BaseModel):
    tempC: float
    water: float
    tdsVal: float
    tdsPpm: float

class SegmentUpdate(BaseModel):
    segments: int

class ActivateAck(BaseModel):
    token: int

def insert_data(data):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    now = time.time()
    c.execute("INSERT INTO sensor_data VALUES (?, ?, ?, ?, ?)", 
              (now, data.tempC, data.water, data.tdsVal, data.tdsPpm))
    cutoff = now - SECONDS_1_YEAR
    c.execute("DELETE FROM sensor_data WHERE time < ?", (cutoff,))
    conn.commit()
    conn.close()

@app.post("/data")
async def receive_data(data: SensorData):
    if load_measurement_state() == 1:
        insert_data(data)
        return {"status": "saved"}
    else:
        return {"status": "paused"}

@app.post("/segments")
async def update_segments(data: SegmentUpdate):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE feeder_state SET segments = ? WHERE id = 1", (data.segments,))
    conn.commit()
    conn.close()
    return {"status": "segments updated"}

@app.get("/segments")
async def get_segments():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT segments FROM feeder_state WHERE id = 1")
    result = c.fetchone()
    conn.close()
    return {"segments": result[0] if result else 0}

@app.get("/activate")
async def activate():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT pending, command_token, acknowledged_token FROM feed_commands WHERE id = 1")
    result = c.fetchone()
    pending = False
    token = 0
    if result:
        pending = bool(result[0] == 1)
        token = int(result[1] or 0)
        acknowledged_token = int(result[2] or 0)
        pending = pending and token > acknowledged_token
    conn.close()
    return {"activate": pending, "token": token}

@app.post("/activate_ack")
async def activate_ack(data: ActivateAck):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT command_token FROM feed_commands WHERE id = 1")
    result = c.fetchone()
    current_token = int(result[0] or 0) if result else 0
    if data.token == current_token and current_token > 0:
        c.execute(
            "UPDATE feed_commands SET pending = 0, acknowledged_token = ? WHERE id = 1",
            (data.token,)
        )
        conn.commit()
        conn.close()
        return {"status": "acknowledged", "token": data.token}
    conn.close()
    return {"status": "ignored", "token": data.token}

def load_feeder_state():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT segments FROM feeder_state WHERE id = 1")
    result = c.fetchone()
    conn.close()
    return result[0] if result else 0

def save_feeder_state(value):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE feeder_state SET segments = ? WHERE id = 1", (value,))
    conn.commit()
    conn.close()

def queue_manual_feeding():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE feed_commands SET pending = 1, command_token = COALESCE(command_token, 0) + 1 WHERE id = 1")
    conn.commit()
    conn.close()
    print("Manual feeding queued")

def save_max_water_level(value):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE water_settings SET max_level = ? WHERE id = 1", (value,))
    conn.commit()
    conn.close()

def save_min_distance(value):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE water_settings SET min_distance = ? WHERE id = 1", (value,))
    conn.commit()
    conn.close()

def load_water_settings():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT max_level, min_distance FROM water_settings WHERE id = 1")
    result = c.fetchone()
    conn.close()
    return result if result else (None, 0)

def load_temperature_settings():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT max_temp, min_temp FROM water_settings WHERE id = 1")
    result = c.fetchone()
    conn.close()
    return result if result else (30.0, 24.0)

def save_max_temperature(value):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE water_settings SET max_temp = ? WHERE id = 1", (value,))
    conn.commit()
    conn.close()

def save_min_temperature(value):
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE water_settings SET min_temp = ? WHERE id = 1", (value,))
    conn.commit()
    conn.close()

def send_discord_warning(message):
    try:
        requests.post(DISCORD_WEBHOOK_URL, json={"content": message})
    except Exception as e:
        print("Discord notification failed:", e)

def load_measurement_state():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("SELECT enabled FROM measurement_state WHERE id = 1")
    result = c.fetchone()
    conn.close()
    return result[0] if result else 1

def toggle_measurement_state():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    current = load_measurement_state()
    new_state = 0 if current == 1 else 1
    c.execute("UPDATE measurement_state SET enabled = ? WHERE id = 1", (new_state,))
    conn.commit()
    conn.close()
    return new_state

# ---------- START API ----------
def start_api():
    uvicorn.run(app, host="0.0.0.0", port=8000)

if "api_started" not in st.session_state:
    threading.Thread(target=start_api, daemon=True).start()
    st.session_state.api_started = True

# ---------- STREAMLIT UI ----------
with st.sidebar:
    st.write(f"👤 **{st.user.name}**")
    st.write(f"📧 {st.user.email}")
    if st.button("Log out"):
        st.logout()

st.title("ESP32 Sensor Dashboard")
st_autorefresh(interval=1000, key="refresh")

if "water_alert_sent" not in st.session_state:
    st.session_state.water_alert_sent = False
if "temp_alert_sent" not in st.session_state:
    st.session_state.temp_alert_sent = False


measurement_enabled = load_measurement_state()

# invert what the UI shows (so left/right meaning is reversed)
ui_value = not bool(measurement_enabled)

new_ui_state = st.toggle(
    "Disable measuring data",
    value=ui_value,
    key="measure_toggle"
)

# convert UI state back to real DB meaning (invert again)
new_db_state = 0 if new_ui_state else 1

# detect change
if new_db_state != measurement_enabled:
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute("UPDATE measurement_state SET enabled = ? WHERE id = 1", (new_db_state,))
    conn.commit()
    conn.close()
    st.rerun()

if new_db_state == 0:
    st.error("Data measuring is DISABLED")


TOTAL_SEGMENTS = 18
segments_value = load_feeder_state()

# ---------- CAMERA ----------
st.subheader("Live Camera Stream")
st.image(CAMERA_URL, width=640)

# ---------- LOAD DATA ----------
def load_recent_data():
    conn = sqlite3.connect(DB_FILE)
    cutoff = time.time() - SECONDS_1_YEAR
    df = pd.read_sql_query("SELECT * FROM sensor_data WHERE time >= ? ORDER BY time",
                           conn, params=(cutoff,))
    conn.close()
    return df

df = load_recent_data()
if df.empty:
    st.info("Waiting for data from ESP32...")
    st.stop()

# ---------- NORMALIZE TIME ----------
# keep a datetime column for plotting on x-axis
df["timestamp"] = pd.to_datetime(df["time"], unit='s')

df["time"] = df["time"] - df["time"].iloc[0]
df = df.set_index("time")

# ---------- LATEST VALUES ----------
latest = df.iloc[-1]
st.subheader("Measured Values")
max_level, min_distance = load_water_settings()
# load temperature settings early so we can display warnings under the heading
max_temp, min_temp = load_temperature_settings()

# Prepare temperature warning message to display under the heading
temp_warning_message = None
current_temp_preview = None
try:
    current_temp_preview = float(df.iloc[-1]["tempC"])
except Exception:
    current_temp_preview = None

if current_temp_preview is not None and max_temp is not None and min_temp is not None:
    if current_temp_preview > max_temp:
        temp_warning_message = f"Water temperature is ABOVE maximum! Current: {current_temp_preview:.2f} °C (Max: {max_temp:.2f} °C)"
    elif current_temp_preview < min_temp:
        temp_warning_message = f"Water temperature is BELOW minimum! Current: {current_temp_preview:.2f} °C (Min: {min_temp:.2f} °C)"

if temp_warning_message:
    st.error(temp_warning_message)

# ---------- DISCORD ALERT LOGIC ----------

if max_level is not None:
    current_water = latest["water"]
    min_allowed = max_level - min_distance

    # LOW WATER CONDITION
    if current_water < min_allowed:
        st.error("Water level is BELOW minimum!")

        if not st.session_state.water_alert_sent:
            send_discord_warning(
                f"Aquarium warning!\n"
                f"Water level is LOW!\n"
                f"Current: {current_water:.2f} cm\n"
                f"Minimum allowed: {min_allowed:.2f} cm"
            )
            st.session_state.water_alert_sent = True

    # BACK TO NORMAL CONDITION
    else:
        # reset alert flag without sending a discord message
        st.session_state.water_alert_sent = False

    

st.markdown(f"""
    **Filled segments:** {segments_value} / {TOTAL_SEGMENTS}<br>
    **Water temperature:** {latest["tempC"]:.2f} °C<br>
""", unsafe_allow_html=True)

# --- TEMPERATURE SETTINGS ---
max_temp, min_temp = load_temperature_settings()

col_t1, col_t2 = st.columns(2)

with col_t1:
    min_temp_input = st.number_input(
        "Min water temperature [°C]",
        min_value=0.0,
        step=0.1,
        value=float(min_temp) if min_temp else 24.0
    )
    if min_temp_input != min_temp:
        save_min_temperature(min_temp_input)

with col_t2:
    max_temp_input = st.number_input(
        "Max water temperature [°C]",
        min_value=0.0,
        step=0.1,
        value=float(max_temp) if max_temp else 30.0
    )
    if max_temp_input != max_temp:
        save_max_temperature(max_temp_input)

# ---------- TEMPERATURE ALERT LOGIC (discord only) ----------
current_temp = latest["tempC"]
if max_temp is not None and min_temp is not None:
    if current_temp > max_temp:
        if not st.session_state.temp_alert_sent:
            send_discord_warning(
                f"Aquarium warning!\n"
                f"Water temperature is HIGH!\n"
                f"Current: {current_temp:.2f} °C\n"
                f"Maximum allowed: {max_temp:.2f} °C"
            )
            st.session_state.temp_alert_sent = True
    elif current_temp < min_temp:
        if not st.session_state.temp_alert_sent:
            send_discord_warning(
                f"Aquarium warning!\n"
                f"Water temperature is LOW!\n"
                f"Current: {current_temp:.2f} °C\n"
                f"Minimum allowed: {min_temp:.2f} °C"
            )
            st.session_state.temp_alert_sent = True
    else:
        # reset temp alert flag without sending a discord message
        st.session_state.temp_alert_sent = False

# --- WATER LEVEL + BUTTON ---
col_w1, col_w2 = st.columns([3, 2])

with col_w1:
    st.markdown(f"**Water level:** {latest['water']} cm")

    # Manual input for max water level (placed under the reading)
    max_level_input = st.number_input(
        "Max water level [cm]",
        min_value=0.0,
        step=1.0,
        value=float(max_level) if max_level else 0.0
    )

    # Save automatically when changed
    if max_level_input != max_level and max_level_input > 0:
        save_max_water_level(max_level_input)
        max_level = max_level_input
        st.rerun()

with col_w2:
    if st.button("Set current water level as max"):
        save_max_water_level(latest["water"])
        st.success("Max water level saved")

# --- INPUT FOR MIN DISTANCE (NUMBERS ONLY) ---
min_distance_input = st.number_input(
    "Min water level distance [cm]",
    min_value=0.0,
    step=1.0,
    value=float(min_distance) if min_distance else 0.0
)

# Save automatically when changed
if min_distance_input != min_distance:
    save_min_distance(min_distance_input)
    min_distance = min_distance_input

# --- CALCULATE MIN LEVEL ---
if max_level is not None:
    min_allowed = max_level - min_distance

    st.markdown(
        f"<span style='color:gray'>The min allowed water level is: {min_allowed:.1f} cm</span>",
        unsafe_allow_html=True
    )
else:
    st.markdown(
        "<span style='color:gray'>Set max water level first</span>",
        unsafe_allow_html=True
    )

# --- TDS ---
st.markdown(f"""
    **TDS:** {int(latest["tdsVal"])} (~{int(latest["tdsPpm"])} ppm)
""", unsafe_allow_html=True)

# ---------- PIE ----------
st.subheader("Food Level")
filled = segments_value
empty = TOTAL_SEGMENTS - filled

def draw_pie_chart(filled, total_segments):
    empty = total_segments - filled
    colors = ["#63A2FF", "#21214D"]  # filled, empty

    fig, ax = plt.subplots(figsize=(1.5, 1.5))  # smaller figure size

    def autopct_generator(values):
        def inner(pct):
            index = inner.counter
            inner.counter += 1
            val = values[index]
            if val == 0:
                return ""  # hide if value is 0
            if index == 0:  # filled
                return f"{val}"
            else:  # empty
                return f"{val}"
        inner.counter = 0
        return inner

    # Main pie slices without labels
    wedges, texts, autotexts = ax.pie(
        [filled, empty],
        labels=[None, None],
        colors=colors,
        autopct=autopct_generator([filled, empty]),
        startangle=90,
        counterclock=False,       # <-- reverse rotation direction
        wedgeprops={"edgecolor": "black", "linewidth": 0.5}
    )

    # Set colors for inside numbers
    if filled > 0:
        autotexts[0].set_color("black")  # black on light blue
    if empty > 0:
        autotexts[1].set_color("white")  # white on dark blue

    # Draw segment lines
    angle_step = 360 / total_segments
    for i in range(total_segments):
        angle_rad = math.radians(90 - i * angle_step)
        x = math.cos(angle_rad)
        y = math.sin(angle_rad)
        ax.plot([0, x], [0, y], color="black", linewidth=0.5)

    # Fix axes so pie doesn't resize
    ax.set_aspect('equal')
    ax.set_xlim(-1.1, 1.1)
    ax.set_ylim(-1.1, 1.1)

    # Keep title
    # Title and legend are displayed in the Streamlit layout above the chart

    plt.tight_layout(pad=0.1)
    st.pyplot(fig)

# Usage
filled = segments_value
empty = TOTAL_SEGMENTS - filled

# Centered filled counter (larger font than legend)
st.markdown(
    f"<div style='text-align:center; font-size:18px; font-weight:bold; margin-top:2px; margin-bottom:0px;'>{filled}/{TOTAL_SEGMENTS} segments</div>",
        unsafe_allow_html=True,
)

# Centered legend stacked vertically (slightly smaller than counter)
st.markdown(
        f"""
        <div style='text-align:center; font-size:14px; margin-top:0px; margin-bottom:0px;'>
            <div style='margin-bottom:2px;'>
                <span style='display:inline-block; width:14px; height:14px; background:#63A2FF; border-radius:50%; margin-right:8px; vertical-align:middle;'></span>
                Filled [{filled}]
            </div>
            <div>
                <span style='display:inline-block; width:14px; height:14px; background:#21214D; border-radius:50%; margin-right:8px; vertical-align:middle;'></span>
                Empty [{empty}]
            </div>
        </div>
        """,
        unsafe_allow_html=True,
)

draw_pie_chart(segments_value, TOTAL_SEGMENTS)

col1, col2, col3 = st.columns(3)
with col1:
    if st.button("➕"):
        if segments_value < TOTAL_SEGMENTS:
            segments_value += 1
            requests_post_segments = {"segments": segments_value}
            import requests
            requests.post("http://192.168.1.121:8000/segments", json=requests_post_segments)
with col2:
    if st.button("➖"):
        if segments_value > 0:
            segments_value -= 1
            requests_post_segments = {"segments": segments_value}
            import requests
            requests.post("http://192.168.1.121:8000/segments", json=requests_post_segments)
with col3:
    if st.button("𝐅𝐔𝐋𝐋/𝐄𝐌𝐏𝐓𝐘"):
        segments_value = TOTAL_SEGMENTS
        requests_post_segments = {"segments": segments_value}
        import requests
        requests.post("http://192.168.1.121:8000/segments", json=requests_post_segments)

if st.button("Manual Feeding", width='stretch'):
    if segments_value > 0:
        segments_value -= 1
        save_feeder_state(segments_value)
        queue_manual_feeding()
        st.success("Manual feeding queued")
        st.rerun()
    else:
        st.warning("No feed segments left")

# ---------- GRAPHS ----------
st.subheader("Sensor Graphs")
tab_temp, tab_water, tab_tdsVal, tab_tdsPpm, tab_all = st.tabs(
    ["Temperature", "Water Level", "TDS Raw", "TDS ppm", "All"])

# Color mapping
color_map = {
    "Temperature": "#FF4136",  # red
    "Water Level": "#1E90FF",  # blue
    "TDS RAW": "#63A2FF",      # light blue
    "TDS PPM": "#2ECC40",      # green
}

with tab_temp:
    df_temp = df.set_index("timestamp")[['tempC']].reset_index()
    chart = alt.Chart(df_temp).mark_line(color=color_map['Temperature']).encode(
        x=alt.X('timestamp:T', title='Time'),
        y=alt.Y('tempC:Q', title='Temperature (°C)')
    )
    st.altair_chart(chart, width='stretch')

with tab_water:
    df_water = df.set_index("timestamp")[['water']].reset_index()
    chart = alt.Chart(df_water).mark_line(color=color_map['Water Level']).encode(
        x=alt.X('timestamp:T', title='Time'),
        y=alt.Y('water:Q', title='Water level (cm)')
    )
    st.altair_chart(chart, width='stretch')

with tab_tdsVal:
    df_tdsval = df.set_index("timestamp")[['tdsVal']].reset_index()
    chart = alt.Chart(df_tdsval).mark_line(color=color_map['TDS RAW']).encode(
        x=alt.X('timestamp:T', title='Time'),
        y=alt.Y('tdsVal:Q', title='TDS Raw')
    )
    st.altair_chart(chart, width='stretch')

with tab_tdsPpm:
    df_tdsp = df.set_index("timestamp")[['tdsPpm']].reset_index()
    chart = alt.Chart(df_tdsp).mark_line(color=color_map['TDS PPM']).encode(
        x=alt.X('timestamp:T', title='Time'),
        y=alt.Y('tdsPpm:Q', title='TDS ppm')
    )
    st.altair_chart(chart, width='stretch')

with tab_all:
    df_all = df.set_index("timestamp")[["tempC", "water", "tdsVal", "tdsPpm"]].rename(
        columns={
            "tempC": "Temperature",
            "water": "Water Level",
            "tdsVal": "TDS RAW",
            "tdsPpm": "TDS PPM",
        }
    ).reset_index()
    df_melt = df_all.melt(id_vars='timestamp', var_name='variable', value_name='value')
    chart = alt.Chart(df_melt).mark_line().encode(
        x=alt.X('timestamp:T', title='Time'),
        y=alt.Y('value:Q', title='Value'),
        color=alt.Color('variable:N', scale=alt.Scale(
            domain=['Temperature', 'Water Level', 'TDS RAW', 'TDS PPM'],
            range=[color_map['Temperature'], color_map['Water Level'], color_map['TDS RAW'], color_map['TDS PPM']]
        ), legend=alt.Legend(title=None))
    )
    st.altair_chart(chart, width='stretch')