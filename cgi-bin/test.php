<?php
header("Content-Type: text/html; charset=utf-8");
?>
<!DOCTYPE html>
<html>
<head>
  <title>PHP CGI Execution</title>
  <style>
    body { font-family: monospace; background: #0f172a; color: #a78bfa; padding: 20px; }
    .card { background: #1e293b; padding: 15px; border-radius: 8px; margin-bottom: 10px; }
  </style>
</head>
<body>
  <h2>PHP CGI Execution Succeeded!</h2>
  <div class="card">
    <strong>Timestamp:</strong> <?php echo date('Y-m-d H:i:s'); ?><br>
    <strong>Request Method:</strong> <?php echo htmlspecialchars($_SERVER['REQUEST_METHOD'] ?? 'UNKNOWN', ENT_QUOTES, 'UTF-8'); ?><br>
    <strong>PATH_INFO:</strong> <?php echo htmlspecialchars($_SERVER['PATH_INFO'] ?? 'None', ENT_QUOTES, 'UTF-8'); ?><br>
    <strong>Query String:</strong> <?php echo htmlspecialchars($_SERVER['QUERY_STRING'] ?? 'None', ENT_QUOTES, 'UTF-8'); ?><br>
    <strong>Cookies:</strong> <?php echo htmlspecialchars(json_encode($_COOKIE), ENT_QUOTES, 'UTF-8'); ?><br>
  </div>
  <div class="card">
    <strong>Received Parameters:</strong>
    <pre><?php echo htmlspecialchars(print_r($_REQUEST, true), ENT_QUOTES, 'UTF-8'); ?></pre>
  </div>
</body>
</html>
