// 01-Edu Localhost Server Suite Client Logic
document.addEventListener('DOMContentLoaded', () => {
  // Update client time clock
  setInterval(() => {
    const el = document.getElementById('client-time');
    if (el) el.textContent = new Date().toLocaleTimeString();
  }, 1000);

  updateCookiesDisplay();
  setupConsole();
  setupForms();
  setupUploads();
  setupFileTable();
  setupCookies();
  setupCgi();
});

// Logging helper
function logToConsole(message, type = 'system') {
  const consoleBox = document.getElementById('http-console');
  if (!consoleBox) return;
  const time = new Date().toISOString().substring(11, 19);
  const entry = document.createElement('div');
  entry.className = `log-entry ${type}`;
  entry.textContent = `[${time}] ${message}`;
  consoleBox.appendChild(entry);
  consoleBox.scrollTop = consoleBox.scrollHeight;
}

function setupConsole() {
  const clearBtn = document.getElementById('btn-clear-console');
  if (clearBtn) {
    clearBtn.addEventListener('click', () => {
      const consoleBox = document.getElementById('http-console');
      if (consoleBox) consoleBox.innerHTML = '';
    });
  }
}

function ensureMethodResult() {
  let result = document.getElementById('method-result');
  if (result) return result;

  const postForm = document.getElementById('post-json-form');
  if (!postForm) return null;

  const wrapper = document.createElement('div');
  wrapper.style.marginTop = '1rem';

  const title = document.createElement('h4');
  title.textContent = 'Last HTTP Response';

  result = document.createElement('pre');
  result.id = 'method-result';
  result.className = 'code-box pre-wrap';
  result.textContent = 'Run GET or POST to inspect the response here.';

  wrapper.append(title, result);
  postForm.insertAdjacentElement('afterend', wrapper);
  return result;
}

function showMethodResponse(label, response, text) {
  const result = ensureMethodResult();
  if (!result) return;

  const details = [
    `${label}`,
    `HTTP ${response.status} ${response.statusText}`,
  ];
  const contentType = response.headers.get('content-type');
  const contentLength = response.headers.get('content-length');
  if (contentType) details.push(`Content-Type: ${contentType}`);
  if (contentLength) details.push(`Content-Length: ${contentLength}`);

  const previewLimit = 800;
  const preview = text.length > previewLimit
    ? `${text.substring(0, previewLimit)}\n… (${text.length - previewLimit} more bytes)`
    : text;

  result.textContent = `${details.join('\n')}\n\n${preview}`;
  result.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
}

// 1. Setup GET and POST forms
function setupForms() {
  const getForm = document.getElementById('get-form');
  const postBtn = document.getElementById('btn-post-json');

  ensureMethodResult();

  if (getForm) {
    getForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const params = new URLSearchParams(new FormData(getForm));
      const target = `/?${params.toString()}`;
      logToConsole(`Sending GET ${target}`, 'get');

      try {
        const response = await fetch(target, { method: 'GET', cache: 'no-store' });
        const text = await response.text();
        history.replaceState(null, '', target);
        showMethodResponse(`GET ${target}`, response, text);
        logToConsole(`GET Response HTTP ${response.status}: ${text.length} bytes`, response.ok ? 'get' : 'error');
      } catch (err) {
        const result = ensureMethodResult();
        if (result) result.textContent = `GET ${target}\nRequest failed: ${err.message}`;
        logToConsole(`GET Error: ${err.message}`, 'error');
      }
    });
  }

  if (postBtn) {
    postBtn.addEventListener('click', async () => {
      const payload = document.getElementById('post-payload').value;
      logToConsole(`Sending POST /api/echo with ${payload.length} bytes`, 'post');
      try {
        const response = await fetch('/api/echo', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
          },
          body: payload
        });
        const text = await response.text();
        showMethodResponse('POST /api/echo', response, text);
        logToConsole(`POST Response HTTP ${response.status}: ${text.substring(0, 100)}`, response.ok ? 'post' : 'error');
      } catch (err) {
        const result = ensureMethodResult();
        if (result) result.textContent = `POST /api/echo\nRequest failed: ${err.message}`;
        logToConsole(`POST Error: ${err.message}`, 'error');
      }
    });
  }
}

function ensureUploadResult() {
  let result = document.getElementById('upload-result');
  if (result) return result;

  const uploadForm = document.getElementById('upload-form');
  if (!uploadForm) return null;

  result = document.createElement('div');
  result.id = 'upload-result';
  result.className = 'code-box pre-wrap';
  result.style.marginTop = '0.75rem';
  result.textContent = 'Upload and payload test results will appear here.';
  uploadForm.insertAdjacentElement('afterend', result);
  return result;
}

function showUploadResult(message) {
  const result = ensureUploadResult();
  if (result) result.textContent = message;
}

// 2. Setup Uploads (Multipart & Chunked)
function setupUploads() {
  const standardBtn = document.getElementById('btn-upload-standard');
  const chunkedBtn = document.getElementById('btn-upload-chunked');
  const fileInput = document.getElementById('file-input');
  const dropZone = document.getElementById('drop-zone');

  ensureUploadResult();

  if (fileInput && dropZone) {
    fileInput.addEventListener('change', () => {
      if (fileInput.files.length > 0) {
        dropZone.querySelector('p').textContent = `Selected: ${fileInput.files[0].name} (${Math.round(fileInput.files[0].size / 1024)} KB)`;
      }
    });
  }

  if (standardBtn) {
    standardBtn.addEventListener('click', async () => {
      if (!fileInput || !fileInput.files.length) {
        alert('Please choose a file to upload first.');
        return;
      }
      const file = fileInput.files[0];
      const formData = new FormData();
      formData.append('file', file);

      showUploadResult(`Uploading ${file.name} (${file.size} bytes) via multipart...`);
      logToConsole(`Uploading ${file.name} (${file.size} bytes) via POST /uploads`, 'post');
      try {
        const res = await fetch('/uploads', {
          method: 'POST',
          body: formData
        });
        showUploadResult(`Multipart upload\nHTTP ${res.status} ${res.statusText}\n${res.ok ? 'Accepted and stored.' : 'Request rejected.'}`);
        logToConsole(`Upload Response: HTTP ${res.status} ${res.statusText}`, res.ok ? 'post' : 'error');
        if (res.ok) await refreshFiles();
      } catch (err) {
        showUploadResult(`Multipart upload failed: ${err.message}`);
        logToConsole(`Upload failed: ${err.message}`, 'error');
      }
    });
  }

  if (chunkedBtn) {
    chunkedBtn.addEventListener('click', () => {
      showUploadResult(
        'Browser note: Fetch cannot reliably force HTTP/1.1 Transfer-Encoding: chunked.\n' +
        'The real chunked request is verified by tests/audit.py using a raw TCP socket.'
      );
      logToConsole('Chunked HTTP/1.1 is verified by the raw-socket audit; browsers cannot force the Transfer-Encoding header.', 'system');
    });
  }
}

// Quick payload size tester for 413
window.testPayloadSize = async function(kilobytes) {
  const sizeBytes = kilobytes * 1024;
  const chunk = 'A'.repeat(sizeBytes);
  const target = `/uploads/payload-${kilobytes}kb.txt`;
  showUploadResult(`POST ${target}\nSending ${kilobytes} KB...`);
  logToConsole(`Testing payload limit with ${kilobytes} KB data...`, 'post');
  try {
    const res = await fetch(target, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: chunk
    });
    const expected = kilobytes > 2048 ? '413 Payload Too Large' : 'successful upload';
    showUploadResult(
      `POST ${target}\nHTTP ${res.status} ${res.statusText}\nExpected: ${expected}\nResult: ${res.status === 413 || res.ok ? 'PASS' : 'CHECK'}`
    );
    if (res.status === 413) {
      logToConsole(`HTTP 413 Payload Too Large returned correctly! Server enforced client_max_body_size.`, 'post');
    } else {
      logToConsole(`Server response: HTTP ${res.status} ${res.statusText}`, res.ok ? 'post' : 'error');
      if (res.ok) await refreshFiles();
    }
  } catch (err) {
    showUploadResult(`POST ${target}\nRequest failed: ${err.message}`);
    logToConsole(`Request error: ${err.message}`, 'error');
  }
};

// 3. DELETE Method handling and live autoindex listing
function setupFileTable() {
  const tbody = document.getElementById('files-list');
  const refreshBtn = document.getElementById('btn-refresh-files');

  if (refreshBtn) refreshBtn.addEventListener('click', refreshFiles);
  if (tbody) {
    tbody.addEventListener('click', async (event) => {
      const button = event.target.closest('.btn-delete-file');
      if (!button) return;
      const fileName = button.dataset.file;
      if (!fileName || !confirm(`Execute HTTP DELETE on /uploads/${fileName}?`)) return;

      const url = `/uploads/${encodeURIComponent(fileName)}`;
      logToConsole(`Executing DELETE ${url}`, 'delete');
      try {
        const res = await fetch(url, { method: 'DELETE' });
        logToConsole(`DELETE Response: HTTP ${res.status} ${res.statusText}`, res.ok ? 'delete' : 'error');
        if (res.ok) await refreshFiles();
      } catch (err) {
        logToConsole(`DELETE failed: ${err.message}`, 'error');
      }
    });
  }

  refreshFiles();
}

async function refreshFiles() {
  const tbody = document.getElementById('files-list');
  if (!tbody) return;
  tbody.replaceChildren();

  try {
    const res = await fetch('/uploads/', { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const html = await res.text();
    const doc = new DOMParser().parseFromString(html, 'text/html');
    const names = [...doc.querySelectorAll('a[href]')]
      .map(anchor => anchor.textContent.trim().replace(/\/$/, ''))
      .filter(name => name && name !== '..');

    if (!names.length) {
      const row = document.createElement('tr');
      const cell = document.createElement('td');
      cell.colSpan = 4;
      cell.textContent = 'No uploaded files yet.';
      row.appendChild(cell);
      tbody.appendChild(row);
      return;
    }

    names.forEach(name => addFileToTable(name));
  } catch (err) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = 4;
    cell.textContent = `Unable to load /uploads/: ${err.message}`;
    row.appendChild(cell);
    tbody.appendChild(row);
  }
}

function addFileToTable(filename, size = null) {
  const tbody = document.getElementById('files-list');
  if (!tbody) return;

  const tr = document.createElement('tr');
  tr.dataset.file = filename;

  const nameCell = document.createElement('td');
  nameCell.textContent = filename;

  const sizeCell = document.createElement('td');
  sizeCell.textContent = Number.isFinite(size) ? `${(size / 1024).toFixed(1)} KB` : '—';

  const url = `/uploads/${encodeURIComponent(filename)}`;
  const pathCell = document.createElement('td');
  const code = document.createElement('code');
  code.textContent = url;
  pathCell.appendChild(code);

  const actionsCell = document.createElement('td');
  const getLink = document.createElement('a');
  getLink.href = url;
  getLink.target = '_blank';
  getLink.rel = 'noopener';
  getLink.className = 'btn btn-xs btn-outline';
  getLink.textContent = 'GET';

  const deleteButton = document.createElement('button');
  deleteButton.type = 'button';
  deleteButton.className = 'btn btn-xs btn-danger btn-delete-file';
  deleteButton.dataset.file = filename;
  deleteButton.textContent = 'DELETE';

  actionsCell.append(getLink, document.createTextNode(' '), deleteButton);
  tr.append(nameCell, sizeCell, pathCell, actionsCell);
  tbody.appendChild(tr);
}

// 4. Cookies & Session Management
function setupCookies() {
  const setBtn = document.getElementById('btn-set-cookie');
  const clearBtn = document.getElementById('btn-clear-cookie');
  const userInp = document.getElementById('username-input');

  if (setBtn) {
    setBtn.addEventListener('click', async () => {
      const username = userInp.value || 'auditor';
      logToConsole(`Requesting server session for user "${username}"`, 'get');
      try {
        const res = await fetch(`/session?user=${encodeURIComponent(username)}`);
        const data = await res.json();
        updateCookiesDisplay();
        logToConsole(`Session ${data.session_id}, visits=${data.visits}`, 'system');
      } catch (err) {
        logToConsole(`Session request failed: ${err.message}`, 'error');
      }
    });
  }

  if (clearBtn) {
    clearBtn.addEventListener('click', () => {
      document.cookie = "session_id=; path=/; max-age=0";
      updateCookiesDisplay();
      logToConsole('Cleared cookies session', 'system');
    });
  }
}

function updateCookiesDisplay() {
  const display = document.getElementById('cookie-display');
  const summary = document.getElementById('cookie-summary');
  const badge = document.getElementById('session-status-badge');
  const cookies = document.cookie;

  if (display) display.textContent = cookies ? cookies : '(No cookies set for this origin)';
  if (summary) summary.textContent = cookies ? cookies.split(';').length + ' item(s)' : 'None';
  if (badge) {
    if (cookies.includes('session_id=')) {
      badge.textContent = 'Authenticated Session';
      badge.className = 'badge';
      badge.style.backgroundColor = '#dcfce7';
      badge.style.color = '#166534';
    } else {
      badge.textContent = 'Guest';
      badge.className = 'badge';
      badge.style.backgroundColor = '#f1f5f9';
      badge.style.color = '#64748b';
    }
  }
}

// 5. CGI Execution
function setupCgi() {
  const execBtn = document.getElementById('btn-exec-cgi');
  if (!execBtn) return;

  execBtn.addEventListener('click', async () => {
    const script = document.getElementById('cgi-script-select').value;
    const method = document.getElementById('cgi-method').value;
    const query = document.getElementById('cgi-query').value;
    const pathInfo = document.getElementById('cgi-path-info').value;
    const resultBox = document.getElementById('cgi-result');

    resultBox.textContent = 'Executing CGI process...';
    const targetUrl = `${script}${pathInfo}${query ? '?' + query : ''}`;

    logToConsole(`Invoking CGI: ${method} ${targetUrl}`, 'get');
    try {
      const options = { method: method };
      if (method === 'POST') {
        options.headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
        options.body = query;
      }
      const res = await fetch(targetUrl, options);
      const text = await res.text();
      resultBox.textContent = `HTTP Status: ${res.status} ${res.statusText}\n\n--- CGI Output ---\n${text}`;
      logToConsole(`CGI executed. Response size: ${text.length} bytes`, 'system');
    } catch (err) {
      resultBox.textContent = `Failed to connect to CGI: ${err.message}\n\nEnsure your Rust/C++ server maps this CGI route.`;
      logToConsole(`CGI Error: ${err.message}`, 'error');
    }
  });
}
