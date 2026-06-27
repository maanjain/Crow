#include "crow.h"
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <sys/statvfs.h>
#include <vector>
#include <cctype>

// Holds the raw numbers we care about from one line of /proc/stat
struct CpuTimes {
    long long user, nice, system, idle, iowait, irq, softirq, steal;

    long long total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
};

// Reads the first line of /proc/stat ("cpu  ...") and parses it
CpuTimes readCpuTimes() {
    std::ifstream file("/proc/stat");
    std::string line;
    std::getline(file, line); // first line only, starts with "cpu "

    std::istringstream iss(line);
    std::string label; // will hold "cpu"
    CpuTimes t{};
    iss >> label >> t.user >> t.nice >> t.system >> t.idle
        >> t.iowait >> t.irq >> t.softirq >> t.steal;

    return t;
}

// Takes two snapshots ~200ms apart and computes CPU usage percentage
double getCpuUsagePercent() {
    CpuTimes first = readCpuTimes();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CpuTimes second = readCpuTimes();

    long long totalDelta = second.total() - first.total();
    long long idleDelta = second.idle - first.idle;

    if (totalDelta <= 0) return 0.0; // avoid divide-by-zero edge case

    double usage = (1.0 - (double)idleDelta / (double)totalDelta) * 100.0;
    return usage;
}

// Reads every per-core line from /proc/stat (cpu0, cpu1, cpu2...).
// We skip the first line ("cpu " - the combined summary) since we
// already have getCpuUsagePercent() for that. This returns one
// CpuTimes struct per core, in order.
std::vector<CpuTimes> readAllCoreTimes() {
    std::ifstream file("/proc/stat");
    std::string line;
    std::vector<CpuTimes> cores;

    while (std::getline(file, line)) {
        // Per-core lines look like "cpu0 ...", "cpu1 ...".
        // The combined line is just "cpu " (no digit after "cpu"),
        // and other rows (intr, ctxt, btime...) don't start with "cpu" at all.
        if (line.rfind("cpu", 0) == 0 && line.size() > 3 && isdigit(line[3])) {
            std::istringstream iss(line);
            std::string label;
            CpuTimes t{};
            iss >> label >> t.user >> t.nice >> t.system >> t.idle
                >> t.iowait >> t.irq >> t.softirq >> t.steal;
            cores.push_back(t);
        } else if (line.rfind("cpu", 0) != 0) {
            // Once we hit a non-"cpu" line (e.g. "intr"), all core
            // lines have already been read - stop early.
            break;
        }
    }

    return cores;
}

// Takes two per-core snapshots ~200ms apart and returns a usage
// percentage for each core, in order (core 0 first, etc.)
std::vector<double> getPerCoreCpuUsage() {
    std::vector<CpuTimes> first = readAllCoreTimes();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<CpuTimes> second = readAllCoreTimes();

    std::vector<double> usages;

    for (size_t i = 0; i < first.size() && i < second.size(); i++) {
        long long totalDelta = second[i].total() - first[i].total();
        long long idleDelta = second[i].idle - first[i].idle;

        double usage = 0.0;
        if (totalDelta > 0) {
            usage = (1.0 - (double)idleDelta / (double)totalDelta) * 100.0;
        }
        usages.push_back(usage);
    }

    return usages;
}
// Unlike CPU, memory values are instantaneous (not cumulative since boot),
// so we only need a single read - no delta/sleep needed.
double getRamUsagePercent() {
    std::ifstream file("/proc/meminfo");
    std::string line;

    long long memTotal = 0, memAvailable = 0;
    int found = 0;

    while (std::getline(file, line) && found < 2) {
        std::istringstream iss(line);
        std::string label;
        long long value;
        iss >> label >> value; // e.g. "MemTotal:" 7906808

        if (label == "MemTotal:") {
            memTotal = value;
            found++;
        } else if (label == "MemAvailable:") {
            memAvailable = value;
            found++;
        }
    }

    if (memTotal <= 0) return 0.0; // avoid divide-by-zero edge case

    double used = memTotal - memAvailable;
    double usage = (used / (double)memTotal) * 100.0;
    return usage;
}

// Reads disk space usage for the root filesystem ("/") using statvfs.
// statvfs gives space info (total/free blocks), unlike /proc/diskstats
// which tracks I/O activity - we want capacity used, not I/O speed.
double getDiskUsagePercent() {
    struct statvfs stat;

    if (statvfs("/", &stat) != 0) {
        return 0.0; // call failed, e.g. permissions issue
    }

    // f_blocks = total blocks, f_bfree = free blocks (including reserved
    // for root), f_bavail = free blocks available to unprivileged users.
    // We use f_bavail for a "real world" available-space number.
    unsigned long long total = stat.f_blocks * stat.f_frsize;
    unsigned long long available = stat.f_bavail * stat.f_frsize;
    unsigned long long used = total - available;

    if (total == 0) return 0.0;

    double usage = ((double)used / (double)total) * 100.0;
    return usage;
}

// Reads /proc/uptime, which looks like "123456.78 98765.43"
// (first number = seconds since boot, second = total idle time across
// all cores combined). We only need the first value.
double getUptimeSeconds() {
    std::ifstream file("/proc/uptime");
    double uptime = 0.0;
    file >> uptime;
    return uptime;
}

int main() {
    crow::SimpleApp app;

    CROW_ROUTE(app, "/")([](){
        return R"(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>sysmon // live dashboard</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.4.1/chart.umd.min.js"></script>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;700&display=swap" rel="stylesheet">
<style>
    :root {
        --bg: #0a0e0c;
        --panel: #101512;
        --line: #1f2b25;
        --text: #d3dbd5;
        --dim: #5d6b62;
        --green: #3ddc84;
        --cyan: #4dd0e1;
        --amber: #ffb74d;
    }
    * { box-sizing: border-box; }
    body {
        background: var(--bg);
        background-image:
            linear-gradient(var(--line) 1px, transparent 1px),
            linear-gradient(90deg, var(--line) 1px, transparent 1px);
        background-size: 32px 32px;
        background-attachment: fixed;
        color: var(--text);
        font-family: 'JetBrains Mono', monospace;
        margin: 0;
        padding: 48px 32px;
        min-height: 100vh;
    }
    .topbar {
        display: flex;
        align-items: baseline;
        justify-content: center;
        gap: 12px;
        margin-bottom: 4px;
    }
    .topbar h1 {
        font-size: 18px;
        font-weight: 700;
        letter-spacing: 0.08em;
        margin: 0;
        text-transform: uppercase;
    }
    .cursor {
        display: inline-block;
        width: 9px;
        height: 16px;
        background: var(--green);
        animation: blink 1.1s steps(1) infinite;
    }
    @keyframes blink { 50% { opacity: 0; } }
    .subline {
        text-align: center;
        color: var(--dim);
        font-size: 12px;
        letter-spacing: 0.05em;
        margin-bottom: 44px;
    }
    .container {
        display: flex;
        gap: 24px;
        justify-content: center;
        flex-wrap: wrap;
        max-width: 1400px;
        margin: 0 auto;
    }
    .card {
        background: var(--panel);
        border: 1px solid var(--line);
        border-radius: 4px;
        padding: 24px 24px 8px;
        width: 400px;
        position: relative;
    }
    .card::before {
        content: '';
        position: absolute;
        top: 0; left: 0;
        width: 100%;
        height: 2px;
        background: var(--accent);
        border-radius: 4px 4px 0 0;
    }
    .card.cpu { --accent: var(--green); }
    .card.ram { --accent: var(--cyan); }
    .card.disk { --accent: var(--amber); }

    .card h2 {
        margin: 0 0 18px;
        font-size: 11px;
        font-weight: 500;
        letter-spacing: 0.12em;
        text-transform: uppercase;
        color: var(--dim);
    }
    .value-row {
        display: flex;
        align-items: baseline;
        gap: 6px;
        margin-bottom: 16px;
    }
    .value {
        font-size: 38px;
        font-weight: 700;
        line-height: 1;
        font-variant-numeric: tabular-nums;
    }
    .unit {
        font-size: 14px;
        color: var(--dim);
    }
    .cpu .value { color: var(--green); }
    .ram .value { color: var(--cyan); }
    .disk .value { color: var(--amber); }

    .status {
        position: fixed;
        bottom: 20px;
        right: 24px;
        font-size: 11px;
        color: var(--dim);
        letter-spacing: 0.05em;
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .dot {
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: var(--green);
        box-shadow: 0 0 6px var(--green);
    }
    .core-row {
        display: flex;
        align-items: center;
        gap: 12px;
        margin-bottom: 10px;
        font-size: 11px;
    }
    .core-label {
        width: 48px;
        color: var(--dim);
        flex-shrink: 0;
    }
    .core-track {
        flex: 1;
        height: 8px;
        background: #161b18;
        border-radius: 3px;
        overflow: hidden;
    }
    .core-fill {
        height: 100%;
        background: var(--green);
        border-radius: 3px;
        transition: width 0.3s ease;
    }
    .core-pct {
        width: 44px;
        text-align: right;
        color: var(--text);
        flex-shrink: 0;
        font-variant-numeric: tabular-nums;
    }
</style>
</head>
<body>

<div class="topbar">
    <h1>sysmon</h1>
    <span class="cursor"></span>
</div>
<div class="subline">live system metrics &mdash; read directly from /proc and statvfs by a c++ binary</div>
<div class="subline" id="uptimeLine">uptime: --</div>

<div class="container">
    <div class="card cpu">
        <h2>cpu.usage</h2>
        <div class="value-row">
            <span class="value" id="cpuValue">0.0</span>
            <span class="unit">%</span>
        </div>
        <canvas id="cpuChart"></canvas>
    </div>

    <div class="card ram">
        <h2>mem.usage</h2>
        <div class="value-row">
            <span class="value" id="ramValue">0.0</span>
            <span class="unit">%</span>
        </div>
        <canvas id="ramChart"></canvas>
    </div>

    <div class="card disk">
        <h2>disk.usage &mdash; /</h2>
        <div class="value-row">
            <span class="value" id="diskValue">0.0</span>
            <span class="unit">%</span>
        </div>
        <canvas id="diskChart"></canvas>
    </div>
</div>

<div class="container" style="margin-top: 24px;">
    <div class="card" style="--accent: var(--green); width: 100%; max-width: 856px;">
        <h2>cpu.cores</h2>
        <div id="coreBars"></div>
    </div>
</div>

<div class="status"><span class="dot"></span>polling /stats every 1s</div>

<script>
const MAX_POINTS = 30; // how many seconds of history to show

function makeChart(ctx, color) {
    return new Chart(ctx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{
                data: [],
                borderColor: color,
                backgroundColor: color + '22',
                fill: true,
                tension: 0.25,
                pointRadius: 0,
                borderWidth: 1.5
            }]
        },
        options: {
            animation: false,
            responsive: true,
            scales: {
                y: {
                    min: 0, max: 100,
                    grid: { color: '#1f2b25' },
                    ticks: { color: '#5d6b62', font: { family: 'JetBrains Mono', size: 10 } }
                },
                x: { display: false }
            },
            plugins: { legend: { display: false } }
        }
    });
}

const cpuChart = makeChart(document.getElementById('cpuChart').getContext('2d'), '#3ddc84');
const ramChart = makeChart(document.getElementById('ramChart').getContext('2d'), '#4dd0e1');
const diskChart = makeChart(document.getElementById('diskChart').getContext('2d'), '#ffb74d');

function pushData(chart, value) {
    const now = new Date().toLocaleTimeString();
    chart.data.labels.push(now);
    chart.data.datasets[0].data.push(value);

    if (chart.data.labels.length > MAX_POINTS) {
        chart.data.labels.shift();
        chart.data.datasets[0].data.shift();
    }
    chart.update();
}

function formatUptime(totalSeconds) {
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);

    const parts = [];
    if (days > 0) parts.push(days + 'd');
    parts.push(hours + 'h');
    parts.push(minutes + 'm');
    return parts.join(' ');
}

// Builds/updates one bar row per core. On the first call it creates
// the DOM elements; on later calls it just updates width + text,
// so we're not rebuilding the whole list every second.
function renderCoreBars(cores) {
    const container = document.getElementById('coreBars');

    if (container.children.length !== cores.length) {
        container.innerHTML = '';
        cores.forEach((_, i) => {
            const row = document.createElement('div');
            row.className = 'core-row';
            row.innerHTML = `
                <span class="core-label">core${i}</span>
                <div class="core-track"><div class="core-fill" id="coreFill${i}"></div></div>
                <span class="core-pct" id="corePct${i}">0.0%</span>
            `;
            container.appendChild(row);
        });
    }

    cores.forEach((value, i) => {
        document.getElementById(`coreFill${i}`).style.width = value.toFixed(1) + '%';
        document.getElementById(`corePct${i}`).textContent = value.toFixed(1) + '%';
    });
}

async function fetchStats() {
    try {
        const res = await fetch('/stats');
        const data = await res.json();

        document.getElementById('cpuValue').textContent = data.cpu_percent.toFixed(1);
        document.getElementById('ramValue').textContent = data.ram_percent.toFixed(1);
        document.getElementById('diskValue').textContent = data.disk_percent.toFixed(1);
        document.getElementById('uptimeLine').textContent = 'uptime: ' + formatUptime(data.uptime_seconds);

        pushData(cpuChart, data.cpu_percent);
        pushData(ramChart, data.ram_percent);
        pushData(diskChart, data.disk_percent);

        if (data.cores) {
            renderCoreBars(data.cores);
        }
    } catch (err) {
        console.error('Failed to fetch stats:', err);
    }
}

setInterval(fetchStats, 1000);
fetchStats(); // initial call so it doesn't wait 1s for first data
</script>

</body>
</html>
        )";
    });

    CROW_ROUTE(app, "/stats")([](){
        double cpuUsage = getCpuUsagePercent();
        double ramUsage = getRamUsagePercent();
        double diskUsage = getDiskUsagePercent();
        double uptime = getUptimeSeconds();
        std::vector<double> coreUsages = getPerCoreCpuUsage();

        crow::json::wvalue result;
        result["cpu_percent"] = cpuUsage;
        result["ram_percent"] = ramUsage;
        result["disk_percent"] = diskUsage;
        result["uptime_seconds"] = uptime;

        // crow::json::wvalue supports array assignment via a vector
        // of wvalue, built manually here for clarity
        std::vector<crow::json::wvalue> coreArray;
        for (double usage : coreUsages) {
            coreArray.push_back(usage);
        }
        result["cores"] = std::move(coreArray);

        return result; //return per function
    });

    app.port(8080).multithreaded().run();
}
