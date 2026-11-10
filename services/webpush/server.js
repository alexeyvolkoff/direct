//push notifications to subscribers

const express = require('express');
const webpush = require('web-push');
const fs = require('fs');

// Read config from external JSON file
const config = JSON.parse(fs.readFileSync('./config/keys.json', 'utf8'));

// setup server
const port = 5000;
const server = express();
server.use(express.json());
server.use(express.static('content'));

// setup vapid keys
const vapidKeys = {
    publicKey: config.vapidKeys.publicKey,
    privateKey: config.vapidKeys.privateKey
};

// set vapid keys for webpush libs
webpush.setVapidDetails('mailto:info@bineon.team', vapidKeys.publicKey, vapidKeys.privateKey);


// add push route
server.post('/push', (req, res) => {

    // subscription request
    let subscription = req.body.subscription;
    let	payload = JSON.stringify(req.body.payload); 
    
    console.log('Got subscription with endpoint: ' + subscription.endpoint);
    console.log('Payload: ' + payload);	

    webpush.sendNotification(subscription, payload).catch(err => console.error(err));	
    // confirm resource created
    res.status(201).json({});

});

server.listen(port, () => console.log(`Push server started at port ${port}`));

