=====================================================================
  AQUAIOTS  -  the website
  Suranjeet Choudhury
=====================================================================

WHAT THESE FILES ARE

  index.html   The whole site: the two-door start screen, the client
               dashboard, your administrator board, and the public story.
  admin.html   Only a redirect to index.html#admin, for old bookmarks.
  api.php      The whole backend in one file. Accounts, sessions,
               device keys, the readings database.
  .htaccess    Stops anyone downloading your database. Do not skip it.

  You do NOT need Node, Python, Docker or a database server.
  Any shared hosting plan with PHP 8.1 or newer runs this.


---------------------------------------------------------------------
STEP 1  -  UPLOAD
---------------------------------------------------------------------

Buy the domain and a basic shared hosting plan - Hostinger, Namecheap,
GoDaddy, anything with cPanel.

Open File Manager in cPanel, go into  public_html , and upload all
four files:

    public_html/index.html
    public_html/admin.html
    public_html/api.php
    public_html/.htaccess

Turn on the free Let's Encrypt SSL certificate in cPanel so the site
is https. The receiver firmware expects https by default.

Visit https://yourdomain.in - the landing page should appear.

If you are UPGRADING from the earlier version of this site, delete the
old  aquaiots.sqlite  as well. The database layout changed and the old
file will not work.


---------------------------------------------------------------------
STEP 2  -  THE DATABASE
---------------------------------------------------------------------

There is nothing to set up. The first time a device or a browser
touches api.php it creates

    aquaiots.sqlite

next to it, with the four tables it needs. That file IS your database -
download it and you have backed up everything.

If your host does not allow SQLite, create a MySQL database in cPanel
and edit the top of api.php:

    const USE_MYSQL   = true;
    const MYSQL_HOST  = 'localhost';
    const MYSQL_DB    = 'yourprefix_aquaiots';
    const MYSQL_USER  = 'yourprefix_aqua';
    const MYSQL_PASS  = 'the password you set in cPanel';

The tables are created automatically either way.

Other settings at the top of api.php you may want to change:

    const KEEP_DAYS   = 90;    // readings older than this are deleted
    const MIN_GAP_SEC = 5;     // a device may not post faster than this
    const ONLINE_SEC  = 180;   // no packet for this long = offline
    const MAX_TRIES   = 6;     // wrong PINs before a client is locked out
    const LOCK_SEC    = 900;   // how long that lockout lasts


---------------------------------------------------------------------
STEP 3  -  CREATE YOUR ADMINISTRATOR, IMMEDIATELY
---------------------------------------------------------------------

Do this before you tell anyone the site exists.

Open  https://yourdomain.in  . The start screen has two sign-in cards,
Client and Administrator. Because no administrator
exists yet, the Administrator card shows a one-time setup form instead
of a login.
Enter your name, your email and a password of at least 10 characters.

The moment you submit it, that form stops working forever. Nobody who
finds the URL afterwards can create a second administrator.

THERE IS NO PASSWORD RESET LINK. Keep the password somewhere safe. If
you lose it, the only way back in is to delete the row from the
admins table in the database by hand.


---------------------------------------------------------------------
STEP 4  -  MAKE THE SITE YOURS
---------------------------------------------------------------------

Open index.html in Notepad and find these three things.

1. YOUR PHOTO. Already in place: your photo is embedded in index.html
   (Founder section). To change it, replace the <img class="shot" ...>
   line there with  <img class="shot" src="suranjeet.jpg" alt="">  and
   upload a square photo of about 400x400 as suranjeet.jpg.

2. YOUR SOCIAL LINKS. In the same founder block there are three
   placeholder addresses, in this order:

       https://facebook.com/
       https://instagram.com/
       https://aquaiots.com/

   Change each to your real page.

3. THE TOWN NAME. Search for  Guwahati, Assam  and change it if you
   want. It appears twice.


---------------------------------------------------------------------
HOW A CLIENT SIGNS IN
---------------------------------------------------------------------

There are no email accounts and no passwords for clients. The receiver
itself prints the credential.

  1. The receiver reaches your site for the first time. Your site has
     never seen that chip before, so it creates a record and hands back
     an eight-character DEVICE KEY, for example  K7M2-9QXA .

  2. The receiver shows that key in a blue bar at the top of its own
     page at 192.168.4.1.

  3. The client opens your website, chooses "Activate a device", types
     the key, and picks a PIN of 4 to 8 digits.

  4. Your site generates their CLIENT ID - for example  AQ-4513  - and
     shows it to them once, in a bordered box, telling them to write it
     down.

  5. From then on they sign in with either the device key or the client
     ID, plus their PIN.

A client with several nodes types whichever key is in front of them;
all of them reach the same account. To add a node later they use the
"Add another node" box with that node's own key.

The device key alphabet leaves out characters people confuse: there is
no letter O and no zero, no letter I and no one. The login field also
tidies up spacing and case by itself, so a key typed as  yluc68v8  on
a phone works the same as  YLUC-68V8 .

Six wrong PINs lock the account for fifteen minutes. A wrong key and a
wrong PIN give exactly the same message, on purpose, so the login form
cannot be used to find out which keys exist.

WHAT NEVER HAPPENS: the site never asks for anyone's WiFi name or
password, never stores one, and has no way to use one. The device key
proves the person is holding the device, which is the only thing that
actually matters.


---------------------------------------------------------------------
WHAT YOU SEE IN THE ADMIN PORTAL
---------------------------------------------------------------------

Per client: their client ID, each node's name and key, whether it is
reporting, when it last reported, whether it is in alert, and whether
it is set to river or tank. Across the top: totals for clients, nodes,
nodes reporting now, and nodes in alert.

What you CANNOT see: water levels, history, or anybody's PIN. The
query that builds that page never touches the readings table, so a
reading cannot reach the admin portal even by accident.

That is a deliberate promise you can make to customers in writing. If
you later decide you need readings for support, it is a one-line
change in api.php - but then you can no longer make the promise.

If a client phones for help, ask for their AQ- number.

FORGOTTEN PINS. There is no self-service reset. When a client forgets
their PIN, ask them to read you the device key off their receiver's
own page - only someone standing next to the device can do that - and
then reset it for them in the database.


---------------------------------------------------------------------
THE FIRMWARE SIDE
---------------------------------------------------------------------

Open node_receiver_8266_cloud.ino and fill in the block at the top:

    #define CLOUD_ENABLE   1
    const char* STA_SSID   = "YOUR-WIFI-NAME";
    const char* STA_PASS   = "YOUR-WIFI-PASSWORD";
    const char* CLOUD_HOST = "aquaiots.in";
    const char* CLOUD_PATH = "/api.php?a=ingest";
    #define CLOUD_HTTPS    1
    #define UPLOAD_SEC     25

  STA_SSID / STA_PASS  the WiFi where the RECEIVER sits. The sender out
  at the water needs no internet at all.
  CLOUD_HOST   just the domain. No https:// and no slash.
  CLOUD_HTTPS  1 if you turned on SSL, otherwise 0.
  UPLOAD_SEC   25 is good. An alert does not wait for the timer.

The ESP8266 only sees 2.4 GHz networks. It cannot join a 5 GHz one.


---------------------------------------------------------------------
IF SOMETHING DOES NOT WORK
---------------------------------------------------------------------

The Serial Monitor prints a status line every five seconds:

    [wifi] ip 192.168.4.1  clients 1  lora ok  wifi 192.168.1.40
           cloud 200  PAIR CODE K7M2-9QXA

  cloud 200   everything is working.
  cloud -1    the receiver has not joined your WiFi. Check the name and
              password, and remember it cannot see 5 GHz.
  cloud -2    could not open the connection. Usually CLOUD_HOST is
              wrong, or CLOUD_HTTPS is 1 but you have no SSL yet.
  cloud -11   timeout. Weak WiFi where the receiver is sitting.
  cloud 404   CLOUD_PATH is wrong, or api.php is not in public_html.
  cloud 401   the token is being stripped. Some hosts drop custom
              headers; if so change CLOUD_PATH to
              "/api.php?a=ingest&token=AQI........" using the token
              the Serial Monitor prints at boot.

If the website says "Server error", switch on PHP error logging in
cPanel and read the log. api.php deliberately never prints database
errors to the browser, because those messages leak table names and
paths, so the log is where they go.

To move host, copy the four files and the .sqlite file across. Nothing
else changes.
