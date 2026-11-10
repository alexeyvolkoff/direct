# syntax=docker/dockerfile:1

FROM node:22-slim

ENV NODE_ENV=production
WORKDIR /app

# Copy package files and install production dependencies
COPY ["package.json", "package-lock.json*", "./"]
RUN npm install --only=production

# Copy server code
COPY ["hub.js", "./"]

# Expose Gun DB Relay + Auth WebSocket port
EXPOSE 8765

CMD ["node", "hub.js"]
