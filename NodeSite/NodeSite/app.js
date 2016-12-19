﻿var httpPort = 80;
var httpsPort = 443;
var localAddress = "0.0.0.0";

var fs = require('fs');
var http = require('http');

var https = require('https');
var credentials;
var privateKey = fs.readFileSync('/etc/letsencrypt/live/beachbevs.com/privkey.pem');
var certificate = fs.readFileSync('/etc/letsencrypt/live/beachbevs.com/fullchain.pem');
var credentials = {
  key: privateKey, cert: certificate
};

var express = require('express');
var app = express();

app.use('/', function (req, res, next) {
  res.header("Access-Control-Allow-Origin", "*");
  res.header("Access-Control-Allow_Headers", "Origin, X-Requested-With, Content-Type, Accept");
  next();
});

app.use(express.static(__dirname + '/public'));


var httpsServer = https.createServer(credentials, app);

httpsServer.listen(httpsPort, localAddress, function () {
  console.log("HTTPS Running");
});

var httpServer = http.createServer(function (req, res) {
  res.writeHead(301, { "Location": "https://" + req.headers['host'] + req.url });
  res.end();
});

httpServer.listen(httpPort, localAddress, function () {
  console.log("HTTP RUNNING");
});
