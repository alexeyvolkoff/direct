#Ubuntu 22.04 LTS
FROM ubuntu:22.04

#Install runtime libraries
RUN apt-get update -y && apt-get install curl -y
RUN curl -fsSL https://deb.nodesource.com/setup_20.x | bash
RUN apt-get install nodejs -y && apt autoremove -y

SHELL ["/bin/bash", "-c"]

#Copy requirenments
COPY server.js /pushserver/server.js
COPY package.json /pushserver/package.json
COPY config/keys.json /pushserver/config/keys.json

#Set working directory to /pushserver
WORKDIR /pushserver

#Install requirenments
RUN bash -c "npm i"


#API
EXPOSE 5000

# Run node
CMD ["node", "./server.js"]
