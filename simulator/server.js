const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');

const PORT = 8080;

const MIME_TYPES = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

const server = http.createServer((req, res) => {
  // CORS proxy for OpenSky API
  if (req.url.startsWith('/api/opensky')) {
    const query = req.url.replace('/api/opensky?', '');
    const apiUrl = `https://opensky-network.org/api/states/all?${query}`;

    console.log(`[Proxy] ${apiUrl}`);

    https.get(apiUrl, (apiRes) => {
      let data = '';
      apiRes.on('data', chunk => data += chunk);
      apiRes.on('end', () => {
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        res.end(data);
      });
    }).on('error', (err) => {
      console.error('[Proxy Error]', err.message);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // Flight route lookup proxy (adsbdb.com - free, no key, better than OpenSky routes)
  if (req.url.startsWith('/api/route/')) {
    const callsign = req.url.replace('/api/route/', '').trim();
    const apiUrl = `https://api.adsbdb.com/v0/callsign/${callsign}`;

    console.log(`[Route] ${apiUrl}`);

    https.get(apiUrl, (apiRes) => {
      let data = '';
      apiRes.on('data', chunk => data += chunk);
      apiRes.on('end', () => {
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        res.end(data);
      });
    }).on('error', (err) => {
      console.error('[Route Error]', err.message);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // Aircraft metadata lookup (hexdb.io - free, no key)
  if (req.url.startsWith('/api/aircraft/')) {
    const hex = req.url.replace('/api/aircraft/', '').trim();
    const apiUrl = `https://hexdb.io/api/v1/aircraft/${hex}`;

    console.log(`[Aircraft] ${apiUrl}`);

    https.get(apiUrl, (apiRes) => {
      let data = '';
      apiRes.on('data', chunk => data += chunk);
      apiRes.on('end', () => {
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        res.end(data);
      });
    }).on('error', (err) => {
      console.error('[Aircraft Error]', err.message);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // Aircraft photo lookup (planespotters.net - free, no key)
  if (req.url.startsWith('/api/photo/')) {
    const hex = req.url.replace('/api/photo/', '').trim();
    const options = {
      hostname: 'api.planespotters.net',
      path: `/pub/photos/hex/${hex}`,
      headers: {
        'User-Agent': 'ESP32FlightRadar/1.0 (+https://github.com/esp32-flight-radar)'
      }
    };

    console.log(`[Photo] https://api.planespotters.net/pub/photos/hex/${hex}`);

    https.get(options, (apiRes) => {
      let data = '';
      apiRes.on('data', chunk => data += chunk);
      apiRes.on('end', () => {
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        res.end(data);
      });
    }).on('error', (err) => {
      console.error('[Photo Error]', err.message);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // Map tile proxy (OpenStreetMap tiles)
  if (req.url.startsWith('/api/tile/')) {
    // Format: /api/tile/z/x/y.png
    const tilePath = req.url.replace('/api/tile/', '');
    const apiUrl = `https://tile.openstreetmap.org/${tilePath}`;

    console.log(`[Tile] ${apiUrl}`);

    const options = {
      hostname: 'tile.openstreetmap.org',
      path: `/${tilePath}`,
      headers: {
        'User-Agent': 'ESP32FlightRadarSimulator/1.0'
      }
    };

    https.get(options, (apiRes) => {
      const chunks = [];
      apiRes.on('data', chunk => chunks.push(chunk));
      apiRes.on('end', () => {
        const buffer = Buffer.concat(chunks);
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'image/png',
          'Access-Control-Allow-Origin': '*',
          'Cache-Control': 'public, max-age=86400',
        });
        res.end(buffer);
      });
    }).on('error', (err) => {
      console.error('[Tile Error]', err.message);
      res.writeHead(502);
      res.end('Tile fetch failed');
    });
    return;
  }

  // UK Postcode lookup proxy (postcodes.io - free, no API key needed)
  if (req.url.startsWith('/api/postcode/')) {
    const postcode = req.url.replace('/api/postcode/', '');
    const apiUrl = `https://api.postcodes.io/postcodes/${postcode}`;

    console.log(`[Postcode] ${apiUrl}`);

    https.get(apiUrl, (apiRes) => {
      let data = '';
      apiRes.on('data', chunk => data += chunk);
      apiRes.on('end', () => {
        res.writeHead(apiRes.statusCode, {
          'Content-Type': 'application/json',
          'Access-Control-Allow-Origin': '*',
        });
        res.end(data);
      });
    }).on('error', (err) => {
      console.error('[Postcode Error]', err.message);
      res.writeHead(502, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // Static file serving
  let filePath = req.url === '/' ? '/index.html' : req.url;
  filePath = path.join(__dirname, filePath);

  const ext = path.extname(filePath);
  const contentType = MIME_TYPES[ext] || 'application/octet-stream';

  fs.readFile(filePath, (err, content) => {
    if (err) {
      res.writeHead(404);
      res.end('Not found');
      return;
    }
    res.writeHead(200, { 'Content-Type': contentType });
    res.end(content);
  });
});

server.listen(PORT, () => {
  console.log(`\n  ✈️  Flight Radar Simulator running at:`);
  console.log(`  → http://localhost:${PORT}\n`);
  console.log(`  API proxy: /api/opensky?lamin=...&lomin=...&lamax=...&lomax=...`);
  console.log(`  Press Ctrl+C to stop\n`);
});
