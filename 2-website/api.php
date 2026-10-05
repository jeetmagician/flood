<?php
/* =====================================================================
   AQUAIOTS  -  api.php
   One file. Device ingest, client portal, admin portal.

   DEVICE
     POST  ingest        device -> server.  Auth: X-Token header
                         `age` = seconds since the receiver last heard the SENSOR
                         over LoRa (-1 = never). Without it the reading is taken
                         as fresh, so older firmware keeps working.
   CLIENT PORTAL
     GET   export        &device=ID&from=TS&to=TS     half-hour history for a PDF report
     POST  claim         {key, pin}          activate a device, makes a client
     POST  login         {id, pin}           id = device key OR client id
     POST  logout
     GET   me
     POST  addkey        {key}               attach another device
     POST  rename        {device, name}
     GET   nodes                             your devices + latest reading
     GET   history       &device=ID&range=live|7d
   ADMIN PORTAL
     POST  admin_setup   {email,password,name}   only while no admin exists
     POST  admin_login   {email,password}
     POST  admin_logout
     GET   admin_me
     GET   admin_clients                     every client, STATUS ONLY
   ===================================================================== */

declare(strict_types=1);

// ---------- configuration --------------------------------------------
const USE_MYSQL   = false;
const MYSQL_HOST  = 'localhost';
const MYSQL_DB    = 'aquaiots';
const MYSQL_USER  = 'aquaiots';
const MYSQL_PASS  = 'change-me';

const SQLITE_FILE = __DIR__ . '/aquaiots.sqlite';
const KEEP_DAYS   = 7;       // half-hour history kept this many days. A year of it is only
                             // ~17,500 rows per device, so raise to 365 for month/year reports.
const HISTORY_SLOT_SEC = 1800; // one history row per device per half hour (:00 and :30)
const LIVE_KEEP_SEC    = 7200; // the live buffer behind the live chart is trimmed to this
const LIVE_CHART_SEC   = 1800; // the live chart shows this much
const MIN_GAP_SEC = 1;       // a device may not post faster than this
const ONLINE_SEC  = 180;     // no packet for this long = offline
const SENSOR_LOST_SEC = 30;  // receiver online but sensor unheard this long = sensor lost
const MAX_TRIES   = 6;       // wrong PINs before a client is locked out
const LOCK_SEC    = 900;     // how long that lockout lasts

// ---------------------------------------------------------------------
header('Content-Type: application/json; charset=utf-8');
header('X-Content-Type-Options: nosniff');

function out(array $data, int $code = 200): never {
    http_response_code($code);
    echo json_encode($data, JSON_UNESCAPED_SLASHES);
    exit;
}
function fail(string $msg, int $code = 400): never { out(['error' => $msg], $code); }

function body(): array {
    $raw = file_get_contents('php://input') ?: '';
    $j = json_decode($raw, true);
    if (is_array($j)) return $j;
    return $_POST;
}

function db(): PDO {
    static $pdo = null;
    if ($pdo instanceof PDO) return $pdo;

    if (USE_MYSQL) {
        $dsn = 'mysql:host=' . MYSQL_HOST . ';dbname=' . MYSQL_DB . ';charset=utf8mb4';
        $pdo = new PDO($dsn, MYSQL_USER, MYSQL_PASS, [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        ]);
        $auto = 'INT AUTO_INCREMENT PRIMARY KEY';
    } else {
        $pdo = new PDO('sqlite:' . SQLITE_FILE, null, null, [
            PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,
            PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        ]);
        $pdo->exec('PRAGMA journal_mode=WAL');
        $auto = 'INTEGER PRIMARY KEY AUTOINCREMENT';
    }

    $pdo->exec("CREATE TABLE IF NOT EXISTS admins (
        id $auto,
        email      VARCHAR(190) UNIQUE,
        pass_hash  VARCHAR(255),
        name       VARCHAR(120),
        created    INT
    )");

    $pdo->exec("CREATE TABLE IF NOT EXISTS clients (
        id $auto,
        code       VARCHAR(16) UNIQUE,   -- AQ-4821, what the admin sees
        pin_hash   VARCHAR(255),
        label      VARCHAR(120),
        tries      INT DEFAULT 0,
        locked     INT DEFAULT 0,
        created    INT,
        last_login INT
    )");

    $pdo->exec("CREATE TABLE IF NOT EXISTS devices (
        id $auto,
        token      VARCHAR(64) UNIQUE,   -- from the chip, never shown
        dev_key    VARCHAR(16) UNIQUE,   -- K7M2-9QXA, shown on the device
        client_id  INT,
        name       VARCHAR(120),
        mode       INT DEFAULT 0,
        depth_mm   INT DEFAULT 0,
        danger_mm  INT DEFAULT 0,
        last_seen  INT,
        created    INT,
        sensor_ts  INT,                  -- when the receiver last heard the sensor
        sensor_ok  INT DEFAULT 1,        -- did the last upload say the sensor is heard?
        lv_level   INT,                  -- the LATEST reading, kept right here so the
        lv_gap     INT,                  -- dashboards and the admin board never need
        lv_alert   INT,                  -- the readings table to know the current state
        lv_rssi    INT,
        lv_snr     REAL,
        lv_ts      INT
    )");

    $pdo->exec("CREATE TABLE IF NOT EXISTS readings (
        id $auto,
        device_id  INT,
        ts         INT,
        level_mm   INT,
        gap_mm     INT,
        depth_mm   INT,
        danger_mm  INT,
        mode       INT,
        alert      INT,
        rssi       INT,
        snr        REAL
    )");
    try { $pdo->exec('CREATE INDEX IF NOT EXISTS ix_read ON readings (device_id, ts)'); }
    catch (PDOException $e) { /* already there */ }

    // readings = the LIVE buffer (every upload, trimmed to LIVE_KEEP_SEC).
    // history  = ONE row per device per half hour, kept KEEP_DAYS, for reports.
    // slot = floor(ts / 1800); UNIQUE(device_id, slot) makes the insert idempotent.
    $pdo->exec("CREATE TABLE IF NOT EXISTS history (
        id $auto,
        device_id  INT,
        slot       INT,
        ts         INT,
        level_mm   INT,
        gap_mm     INT,
        depth_mm   INT,
        danger_mm  INT,
        mode       INT,
        alert      INT,
        rssi       INT,
        snr        REAL,
        UNIQUE (device_id, slot)
    )");

    // A database made by an earlier version lacks newer columns. CREATE TABLE IF NOT
    // EXISTS will not add them, so add each missing one once.
    $have = [];
    if (USE_MYSQL) { foreach ($pdo->query('SHOW COLUMNS FROM devices') as $c) $have[] = $c['Field']; }
    else           { foreach ($pdo->query('PRAGMA table_info(devices)') as $c) $have[] = $c['name']; }
    foreach (['sensor_ts' => 'INT', 'sensor_ok' => 'INT DEFAULT 1',
              'lv_level' => 'INT', 'lv_gap' => 'INT', 'lv_alert' => 'INT',
              'lv_rssi' => 'INT', 'lv_snr' => 'REAL', 'lv_ts' => 'INT'] as $col => $ddl) {
        if (!in_array($col, $have, true)) $pdo->exec("ALTER TABLE devices ADD COLUMN $col $ddl");
    }

    return $pdo;
}

function session_start_safe(): void {
    if (session_status() === PHP_SESSION_ACTIVE) return;
    session_set_cookie_params([
        'lifetime' => 0,
        'path'     => '/',
        'httponly' => true,
        'samesite' => 'Lax',
        'secure'   => (($_SERVER['HTTPS'] ?? '') !== '') ||
                      (($_SERVER['HTTP_X_FORWARDED_PROTO'] ?? '') === 'https'),
    ]);
    session_start();
}

// Ambiguous characters are left out, so nobody has to guess O from 0.
const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

function rand_block(int $n): string {
    $s = '';
    for ($i = 0; $i < $n; $i++) $s .= ALPHABET[random_int(0, strlen(ALPHABET) - 1)];
    return $s;
}
// The key printed on the device: 8 characters, about a trillion combinations.
function make_key(): string  { return rand_block(4) . '-' . rand_block(4); }
// The client's own number, the one the admin reads out on the phone.
function make_code(): string { return 'AQ-' . random_int(1000, 9999); }

function i(array $src, string $k, int $def = 0): int {
    foreach ([$k, $k . '_mm'] as $key) {
        if (isset($src[$key]) && is_numeric($src[$key])) return (int)$src[$key];
    }
    return $def;
}

function tidy_key(string $s): string {
    $s = strtoupper(preg_replace('/[^A-Za-z0-9]/', '', $s) ?? '');
    return strlen($s) === 8 ? substr($s, 0, 4) . '-' . substr($s, 4) : $s;
}

/* A client may sign in with the key printed on any of their devices, or with
   their own client id. Both land on the same row. */
function find_client(PDO $pdo, string $raw): ?array {
    $raw = strtoupper(trim($raw));

    if (preg_match('/^AQ-?(\d{4})$/', $raw, $m)) {
        $s = $pdo->prepare('SELECT * FROM clients WHERE code = ?');
        $s->execute(['AQ-' . $m[1]]);
        return $s->fetch() ?: null;
    }

    $key = tidy_key($raw);
    $s = $pdo->prepare('SELECT client_id FROM devices WHERE dev_key = ?');
    $s->execute([$key]);
    $d = $s->fetch();
    if (!$d || $d['client_id'] === null) return null;

    $s = $pdo->prepare('SELECT * FROM clients WHERE id = ?');
    $s->execute([(int)$d['client_id']]);
    return $s->fetch() ?: null;
}

function client_or_401(PDO $pdo): array {
    session_start_safe();
    $cid = $_SESSION['cid'] ?? null;
    if (!$cid) fail('Not signed in.', 401);
    $s = $pdo->prepare('SELECT * FROM clients WHERE id = ?');
    $s->execute([(int)$cid]);
    $c = $s->fetch();
    if (!$c) fail('Not signed in.', 401);
    return $c;
}

function admin_or_401(PDO $pdo): array {
    session_start_safe();
    $aid = $_SESSION['aid'] ?? null;
    if (!$aid) fail('Not signed in.', 401);
    $s = $pdo->prepare('SELECT * FROM admins WHERE id = ?');
    $s->execute([(int)$aid]);
    $a = $s->fetch();
    if (!$a) fail('Not signed in.', 401);
    return $a;
}

function state_of(?array $r, int $now): string {
    if (!$r || $r['ts'] === null)                 return 'waiting';
    if ($now - (int)$r['ts'] > ONLINE_SEC)        return 'offline';
    if ((int)$r['alert'] === 1)                   return 'alert';
    $lvl = (int)$r['level_mm']; $dang = (int)$r['danger_mm'];
    if ($dang > 0 && $lvl >= $dang * 0.85)        return 'rising';
    return 'normal';
}

// =====================================================================
$a   = $_GET['a'] ?? '';
$pdo = db();
$now = time();

try {
switch ($a) {

/* ---------- the device talks here ------------------------------------ */
case 'ingest': {
    $tok = $_SERVER['HTTP_X_TOKEN'] ?? ($_GET['token'] ?? '');
    $tok = preg_replace('/[^A-Za-z0-9_-]/', '', (string)$tok);
    if (strlen($tok) < 8) fail('Bad device token.', 401);

    $b = body();

    $s = $pdo->prepare('SELECT * FROM devices WHERE token = ?');
    $s->execute([$tok]);
    $dev = $s->fetch();

    if (!$dev) {
        // First time we have heard from this node. Register it unclaimed and
        // hand back the key its owner will type on the site.
        for ($try = 0; $try < 8; $try++) {
            try {
                $pdo->prepare('INSERT INTO devices (token, dev_key, name, created, last_seen)
                               VALUES (?,?,?,?,?)')
                    ->execute([$tok, make_key(), 'New node', $now, $now]);
                break;
            } catch (PDOException $e) { if ($try === 7) throw $e; }
        }
        $s->execute([$tok]);
        $dev = $s->fetch();
    } elseif ($now - (int)$dev['last_seen'] < MIN_GAP_SEC) {
        out(['ok' => true, 'throttled' => true, 'code' => $dev['dev_key'],
             'paired' => $dev['client_id'] !== null]);
    }

    // Seconds since the receiver last heard the sensor. Absent = older firmware:
    // treat as fresh. -1 = never heard it. Above SENSOR_LOST_SEC = sensor lost.
    $age   = isset($b['age']) && is_numeric($b['age']) ? (int)$b['age'] : 0;
    $fresh = $age >= 0 && $age <= SENSOR_LOST_SEC;

    if (!$fresh) {
        // The receiver is alive but the sensor is not being heard. Do NOT store
        // its stale numbers as a new reading, or the chart would draw a flat line
        // and the site would look normal. Record only that the receiver is online.
        $pdo->prepare('UPDATE devices SET last_seen=?, sensor_ok=0, sensor_ts=? WHERE id=?')
            ->execute([$now, $age >= 0 ? $now - $age : null, $dev['id']]);
        out(['ok' => true, 'code' => $dev['dev_key'], 'paired' => $dev['client_id'] !== null,
             'sensor' => false]);
    }

    $level  = i($b, 'level', -1);
    $gap    = i($b, 'gap', -1);
    $depth  = max(1, i($b, 'depth', 1));
    $danger = i($b, 'danger');
    $mode   = i($b, 'mode') ? 1 : 0;
    $alert  = i($b, 'alert') ? 1 : 0;
    $rssi   = i($b, 'rssi');
    $snr    = (float)($b['snr'] ?? 0);

    // 1. The live buffer: every upload, so the live chart moves.
    $pdo->prepare('INSERT INTO readings
        (device_id, ts, level_mm, gap_mm, depth_mm, danger_mm, mode, alert, rssi, snr)
        VALUES (?,?,?,?,?,?,?,?,?,?)')
        ->execute([$dev['id'], $now, $level, $gap, $depth, $danger, $mode, $alert, $rssi, $snr]);

    // 2. The latest reading, on the device row itself.
    $pdo->prepare('UPDATE devices SET last_seen=?, mode=?, depth_mm=?, danger_mm=?, sensor_ok=1, sensor_ts=?,
                   lv_level=?, lv_gap=?, lv_alert=?, lv_rssi=?, lv_snr=?, lv_ts=? WHERE id=?')
        ->execute([$now, $mode, $depth, $danger, $now - $age,
                   $level, $gap, $alert, $rssi, $snr, $now, $dev['id']]);

    // 3. The permanent record: the first fresh reading of each half hour. The slot
    //    key makes a second insert in the same half hour a silent no-op.
    $ign = USE_MYSQL ? 'INSERT IGNORE' : 'INSERT OR IGNORE';
    $pdo->prepare("$ign INTO history
        (device_id, slot, ts, level_mm, gap_mm, depth_mm, danger_mm, mode, alert, rssi, snr)
        VALUES (?,?,?,?,?,?,?,?,?,?,?)")
        ->execute([$dev['id'], intdiv($now, HISTORY_SLOT_SEC), $now, $level, $gap, $depth, $danger,
                   $mode, $alert, $rssi, $snr]);

    if (random_int(1, 30) === 1) {
        $pdo->prepare('DELETE FROM readings WHERE ts < ?')->execute([$now - LIVE_KEEP_SEC]);
        $pdo->prepare('DELETE FROM history  WHERE ts < ?')->execute([$now - KEEP_DAYS * 86400]);
    }

    out(['ok' => true, 'code' => $dev['dev_key'], 'paired' => $dev['client_id'] !== null,
         'sensor' => true]);
}

/* ---------- client portal -------------------------------------------- */
case 'claim': {
    $b   = body();
    $key = tidy_key((string)($b['key'] ?? ''));
    $pin = preg_replace('/\D/', '', (string)($b['pin'] ?? ''));

    if (!preg_match('/^[A-Z0-9]{4}-[A-Z0-9]{4}$/', $key))
        fail('That device key does not look right. It has eight characters, like K7M2-9QXA.');
    if (strlen($pin) < 4 || strlen($pin) > 8)
        fail('Choose a PIN of 4 to 8 digits.');

    $s = $pdo->prepare('SELECT * FROM devices WHERE dev_key = ?');
    $s->execute([$key]);
    $dev = $s->fetch();

    if (!$dev)
        fail('No device has reported that key yet. Power the receiver, let it reach the internet, and read the key off its own page at 192.168.4.1.');
    if ($dev['client_id'] !== null)
        fail('That device is already activated. Sign in with its key and your PIN instead.');

    for ($try = 0; $try < 8; $try++) {
        try {
            $pdo->prepare('INSERT INTO clients (code, pin_hash, label, created, last_login)
                           VALUES (?,?,?,?,?)')
                ->execute([make_code(), password_hash($pin, PASSWORD_DEFAULT),
                           trim((string)($b['label'] ?? '')), $now, $now]);
            break;
        } catch (PDOException $e) { if ($try === 7) throw $e; }
    }
    $cid = (int)$pdo->lastInsertId();

    $name = trim((string)($b['name'] ?? '')) ?: 'My node';
    $pdo->prepare('UPDATE devices SET client_id=?, name=? WHERE id=?')
        ->execute([$cid, $name, $dev['id']]);

    $s = $pdo->prepare('SELECT * FROM clients WHERE id = ?');
    $s->execute([$cid]);
    $c = $s->fetch();

    session_start_safe();
    session_regenerate_id(true);
    $_SESSION['cid'] = $cid;

    out(['ok' => true, 'client' => ['code' => $c['code'], 'label' => $c['label']]]);
}

case 'login': {
    $b   = body();
    $id  = (string)($b['id'] ?? '');
    $pin = preg_replace('/\D/', '', (string)($b['pin'] ?? ''));

    $c = find_client($pdo, $id);

    // One message for every kind of failure, so this cannot be used to find
    // out which keys or client ids exist.
    $wrong = 'That key or client ID and PIN do not match.';

    if (!$c) { usleep(300000); fail($wrong, 401); }

    if ((int)$c['locked'] > $now) {
        $mins = (int)ceil(((int)$c['locked'] - $now) / 60);
        fail("Too many wrong PINs. Try again in {$mins} minutes.", 429);
    }

    if (!password_verify($pin, (string)$c['pin_hash'])) {
        $tries = (int)$c['tries'] + 1;
        $lock  = $tries >= MAX_TRIES ? $now + LOCK_SEC : 0;
        $pdo->prepare('UPDATE clients SET tries=?, locked=? WHERE id=?')
            ->execute([$tries >= MAX_TRIES ? 0 : $tries, $lock, $c['id']]);
        usleep(300000);
        fail($wrong, 401);
    }

    $pdo->prepare('UPDATE clients SET tries=0, locked=0, last_login=? WHERE id=?')
        ->execute([$now, $c['id']]);

    session_start_safe();
    session_regenerate_id(true);
    $_SESSION['cid'] = (int)$c['id'];

    out(['ok' => true, 'client' => ['code' => $c['code'], 'label' => $c['label']]]);
}

case 'logout': {
    // Client and admin share one page, so one cookie. Each sign-out removes
    // only its own half of the session.
    session_start_safe();
    unset($_SESSION['cid']);
    out(['ok' => true]);
}

case 'me': {
    $c = client_or_401($pdo);
    out(['ok' => true, 'client' => ['code' => $c['code'], 'label' => $c['label']]]);
}

case 'addkey': {
    $c   = client_or_401($pdo);
    $b   = body();
    $key = tidy_key((string)($b['key'] ?? ''));

    if (!preg_match('/^[A-Z0-9]{4}-[A-Z0-9]{4}$/', $key))
        fail('That device key does not look right.');

    $s = $pdo->prepare('SELECT * FROM devices WHERE dev_key = ?');
    $s->execute([$key]);
    $dev = $s->fetch();

    if (!$dev)              fail('No device has reported that key yet.');
    if ($dev['client_id'] !== null) {
        if ((int)$dev['client_id'] === (int)$c['id']) fail('That device is already on your account.');
        fail('That device belongs to another account.');
    }

    $name = trim((string)($b['name'] ?? '')) ?: 'New node';
    $pdo->prepare('UPDATE devices SET client_id=?, name=? WHERE id=?')
        ->execute([$c['id'], $name, $dev['id']]);

    out(['ok' => true, 'device' => (int)$dev['id']]);
}

case 'rename': {
    $c = client_or_401($pdo);
    $b = body();
    $name = trim((string)($b['name'] ?? ''));
    if ($name === '') fail('Give the node a name.');
    $pdo->prepare('UPDATE devices SET name=? WHERE id=? AND client_id=?')
        ->execute([mb_substr($name, 0, 120), (int)($b['device'] ?? 0), $c['id']]);
    out(['ok' => true]);
}

case 'nodes': {
    $c = client_or_401($pdo);
    $s = $pdo->prepare('SELECT id, dev_key, name, mode, depth_mm, danger_mm, last_seen, sensor_ts, sensor_ok,
                               lv_level, lv_gap, lv_alert, lv_rssi, lv_snr, lv_ts
                        FROM devices WHERE client_id = ? ORDER BY id');
    $s->execute([$c['id']]);
    $rows = $s->fetchAll();

    $last = $pdo->prepare('SELECT * FROM readings WHERE device_id = ? ORDER BY ts DESC LIMIT 1');
    foreach ($rows as &$r) {
        if ($r['lv_ts'] !== null) {
            $r['latest'] = ['ts' => (int)$r['lv_ts'], 'level_mm' => (int)$r['lv_level'], 'gap_mm' => (int)$r['lv_gap'],
                            'depth_mm' => (int)$r['depth_mm'], 'danger_mm' => (int)$r['danger_mm'],
                            'mode' => (int)$r['mode'], 'alert' => (int)$r['lv_alert'],
                            'rssi' => (int)$r['lv_rssi'], 'snr' => (float)$r['lv_snr']];
        } else {                      // a device from before the latest-reading columns existed
            $last->execute([$r['id']]);
            $r['latest'] = $last->fetch() ?: null;
        }
        $r['age']    = $r['last_seen'] ? $now - (int)$r['last_seen'] : null;
        $r['online'] = $r['age'] !== null && $r['age'] < ONLINE_SEC;
        $r['state']  = state_of($r['latest'], $now);
        // Receiver online, but its last upload said the sensor is not heard.
        $r['sensor_lost'] = $r['online'] && $r['sensor_ok'] !== null && (int)$r['sensor_ok'] === 0;
        $r['sensor_age']  = $r['sensor_ts'] ? $now - (int)$r['sensor_ts'] : null;
        if ($r['sensor_lost'] && $r['latest'] !== null) $r['state'] = 'nosensor';
        unset($r['sensor_ts'], $r['sensor_ok'], $r['lv_level'], $r['lv_gap'], $r['lv_alert'],
              $r['lv_rssi'], $r['lv_snr'], $r['lv_ts']);
    }
    unset($r);

    out(['ok' => true, 'client' => ['code' => $c['code']], 'nodes' => $rows, 'now' => $now,
         'keep_days' => KEEP_DAYS]);
}

case 'history': {
    $c   = client_or_401($pdo);
    $dev = (int)($_GET['device'] ?? 0);

    $s = $pdo->prepare('SELECT id FROM devices WHERE id = ? AND client_id = ?');
    $s->execute([$dev, $c['id']]);
    if (!$s->fetch()) fail('Not your device.', 403);

    if (($_GET['range'] ?? 'live') === '7d') {
        $range = '7d';
        $s = $pdo->prepare('SELECT ts, level_mm, alert FROM history WHERE device_id = ? AND ts >= ? ORDER BY ts');
        $s->execute([$dev, $now - KEEP_DAYS * 86400]);
        $rows = $s->fetchAll();
    } else {
        $range = 'live';
        $s = $pdo->prepare('SELECT ts, level_mm, alert FROM readings WHERE device_id = ? AND ts >= ? ORDER BY ts');
        $s->execute([$dev, $now - LIVE_CHART_SEC]);
        $rows = $s->fetchAll();
        // A chart this wide cannot show thousands of points: keep about 150.
        $step = max(1, (int)ceil(count($rows) / 150));
        if ($step > 1) {
            $keep = [];
            foreach ($rows as $k => $r) if ($k % $step === 0 || $k === count($rows) - 1) $keep[] = $r;
            $rows = $keep;
        }
    }
    out(['ok' => true, 'range' => $range, 'keep_days' => KEEP_DAYS, 'rows' => $rows]);
}

/* One device's half-hour history for a PDF report. The browser builds the PDF. */
case 'export': {
    $c    = client_or_401($pdo);
    $dev  = (int)($_GET['device'] ?? 0);
    $from = (int)($_GET['from'] ?? 0);
    $to   = (int)($_GET['to'] ?? 0);
    if ($from <= 0 || $to <= $from || $to - $from > 400 * 86400) fail('Choose a valid period.');

    $s = $pdo->prepare('SELECT name, dev_key, mode, depth_mm, danger_mm FROM devices WHERE id = ? AND client_id = ?');
    $s->execute([$dev, $c['id']]);
    $d = $s->fetch();
    if (!$d) fail('Not your device.', 403);

    $s = $pdo->prepare('SELECT ts, level_mm, gap_mm, depth_mm, danger_mm, alert, rssi, snr FROM history
                        WHERE device_id = ? AND ts >= ? AND ts < ? ORDER BY ts LIMIT 20000');
    $s->execute([$dev, $from, $to]);

    out(['ok' => true, 'client' => ['code' => $c['code']],
         'device' => ['name' => $d['name'], 'dev_key' => $d['dev_key'], 'mode' => (int)$d['mode'],
                      'depth_mm' => (int)$d['depth_mm'], 'danger_mm' => (int)$d['danger_mm']],
         'from' => $from, 'to' => $to, 'generated' => $now, 'keep_days' => KEEP_DAYS,
         'rows' => $s->fetchAll()]);
}

/* ---------- admin portal --------------------------------------------- */
case 'admin_setup': {
    // Works exactly once, while the admins table is empty. After that it is
    // dead, so nobody can create a second admin by finding this URL.
    $n = (int)$pdo->query('SELECT COUNT(*) c FROM admins')->fetch()['c'];
    if ($n > 0) fail('An administrator already exists.', 403);

    $b     = body();
    $email = strtolower(trim((string)($b['email'] ?? '')));
    $pass  = (string)($b['password'] ?? '');
    if (!filter_var($email, FILTER_VALIDATE_EMAIL)) fail('That email address is not valid.');
    if (strlen($pass) < 10) fail('Use an admin password of at least 10 characters.');

    $pdo->prepare('INSERT INTO admins (email, pass_hash, name, created) VALUES (?,?,?,?)')
        ->execute([$email, password_hash($pass, PASSWORD_DEFAULT),
                   trim((string)($b['name'] ?? '')), $now]);

    session_start_safe();
    session_regenerate_id(true);
    $_SESSION['aid'] = (int)$pdo->lastInsertId();
    out(['ok' => true]);
}

case 'admin_login': {
    $b     = body();
    $email = strtolower(trim((string)($b['email'] ?? '')));
    $pass  = (string)($b['password'] ?? '');

    $s = $pdo->prepare('SELECT * FROM admins WHERE email = ?');
    $s->execute([$email]);
    $ad = $s->fetch();

    if (!$ad || !password_verify($pass, (string)$ad['pass_hash'])) {
        usleep(400000);
        fail('Email or password is wrong.', 401);
    }

    session_start_safe();
    session_regenerate_id(true);
    $_SESSION['aid'] = (int)$ad['id'];
    out(['ok' => true, 'admin' => ['email' => $ad['email'], 'name' => $ad['name']]]);
}

case 'admin_logout': {
    session_start_safe();
    unset($_SESSION['aid']);
    out(['ok' => true]);
}

case 'admin_me': {
    $n = (int)$pdo->query('SELECT COUNT(*) c FROM admins')->fetch()['c'];
    session_start_safe();
    if (!($_SESSION['aid'] ?? null)) out(['ok' => false, 'setup' => $n === 0]);
    $ad = admin_or_401($pdo);
    out(['ok' => true, 'admin' => ['email' => $ad['email'], 'name' => $ad['name']]]);
}

case 'admin_clients': {
    admin_or_401($pdo);

    // STATUS ONLY. Neither this query nor the one below reads the readings or
    // history tables, and neither selects a level, so a water reading cannot
    // reach the admin portal even by accident. The alert flag and the time of the
    // latest upload come from the device row.
    $rows = $pdo->query('SELECT id, code, label, created, last_login FROM clients ORDER BY id DESC')
                ->fetchAll();

    $dq = $pdo->prepare('SELECT id, dev_key, name, mode, last_seen, sensor_ok, lv_alert, lv_ts FROM devices
                         WHERE client_id = ? ORDER BY id');

    $onlineTotal = 0; $alertTotal = 0; $devTotal = 0; $lostTotal = 0;

    foreach ($rows as &$c) {
        $dq->execute([$c['id']]);
        $devs = $dq->fetchAll();
        foreach ($devs as &$d) {
            $d['age']    = $d['last_seen'] ? $now - (int)$d['last_seen'] : null;
            $d['online'] = $d['age'] !== null && $d['age'] < ONLINE_SEC;
            $has  = $d['lv_ts'] !== null;
            $lost = $d['online'] && $d['sensor_ok'] !== null && (int)$d['sensor_ok'] === 0;
            $d['state'] = $has
                ? (!$d['online'] ? 'offline' : ($lost ? 'nosensor' : ((int)$d['lv_alert'] === 1 ? 'alert' : 'normal')))
                : 'waiting';
            unset($d['last_seen'], $d['sensor_ok'], $d['lv_alert'], $d['lv_ts']);
            if ($d['state'] === 'nosensor') $lostTotal++;
            $devTotal++;
            if ($d['online']) $onlineTotal++;
            if ($d['state'] === 'alert') $alertTotal++;
        }
        unset($d);
        $c['devices'] = $devs;
    }
    unset($c);

    out(['ok' => true, 'clients' => $rows, 'now' => $now,
         'totals' => ['clients' => count($rows), 'devices' => $devTotal,
                      'online'  => $onlineTotal, 'alerts' => $alertTotal, 'lost' => $lostTotal]]);
}

default:
    fail('Unknown action.', 404);
}

} catch (Throwable $e) {
    // Never hand a database message to the browser - it leaks table names
    // and paths. The real error goes to the PHP error log.
    error_log('[aquaiots] ' . $e->getMessage());
    fail('Server error.', 500);
}
