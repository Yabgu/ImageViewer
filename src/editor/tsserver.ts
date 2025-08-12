// Simple TypeScript HTTP server for demonstration
import { createServer } from 'http';

const PORT = 8080;

const server = createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/plain' });
  res.end('TypeScript server running!');
});

server.listen(PORT, () => {
  console.log(`Server running at http://localhost:${PORT}/`);
});
