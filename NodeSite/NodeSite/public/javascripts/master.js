﻿'use strict';

var BUCKET_REGION = 'us-west-1';
var BUCKET_NAME = 'beachbev-resumes'

var ACCEPT_ASTATE = 1;

var setman = null;
var masterManager = null;

$('document').ready(function () {
	$("#mBar").load("./mBar.html", function () {
		masterManager = new MasterManager();
		setman = new SetupManager(true, masterManager);
	});
});

function MasterManager() {
	masterManager = this;
	this.s3Client = null;
	this.pdf = null;
	this.pdfIndex = 0;

	this.onProto = function () {
		masterManager.initPackets();
	}

	this.onOpen = function () {
		masterManager.sendE0();
	}

	this.onReopen = function () {
		masterManager.sendE0();
	}

	this.onClose = function () {
		$('#loading').removeClass('hidden');
		$('#empViewDiv').addClass('hidden');
	}

	this.initDisplay = function () {
		$('#loading').addClass('hidden');
		$('#empViewDiv').removeClass('hidden');
	}

	this.setErrorMsg = function () {
		$('#msg').text(msg);
		$('#msg').removeClass('hidden');
		$('#msg').focus();
		$('html, body').scrollTo($('#msg'), 100);
	}

	this.clearErrorMsg = function () {
		$('#msg').addClass('hidden');
	}

	this.initAWS = function (resumeCreds) {
		AWS.config.update({
			credentials: resumeCreds,
			region: BUCKET_REGION
		});
		masterManager.s3Client = new AWS.S3({ apiVersion: '2006-03-01' });
	}

	this.initPackets = function (root) {
		masterManager.PacketD2 = setman.client.root.lookup("ProtobufPackets.PackD2");
		masterManager.PacketD1 = setman.client.root.lookup("ProtobufPackets.PackD1");
		masterManager.PacketE0 = setman.client.root.lookup("ProtobufPackets.PackE0");
		masterManager.PacketE1 = setman.client.root.lookup("ProtobufPackets.PackE1");
		masterManager.PacketE2 = setman.client.root.lookup("ProtobufPackets.PackE2");
		masterManager.PacketE3 = setman.client.root.lookup("ProtobufPackets.PackE3");
		setman.client.packetManager.addPKey(new PKey("E1", function (iPack) {
			var packE1 = masterManager.PacketE1.decode(iPack.packData);
			if (packE1.success) {
				for (var i = 0; i < packE1.acceptedEIDs.length; i++) {
					masterManager.addEmp('#acceptEmpDiv', packE1.acceptedEIDs[i].toString());
				}
				for (var i = 0; i < packE1.unacceptedEIDs.length; i++) {
					masterManager.addEmp('#unacceptEmpDiv', packE1.unacceptedEIDs[i].toString());
					$('#' + packE1.unacceptedEIDs[i].toString() + ' > div > .acceptButton').removeClass('hidden');
				}
				masterManager.sendD2();
			}
			else
			{
				console.log(packE1.msg);
			}
		}));
		setman.client.packetManager.addPKey(new PKey("D1", function (iPack) {
			var packD1 = masterManager.PacketD1.decode(iPack.packData);
			if (packD1.accessKey.length > 0) {
				var credentials = new AWS.Credentials(packD1.accessKeyID, packD1.accessKey, packD1.sessionKey);
				masterManager.initAWS(credentials);
				masterManager.setHasResumes();
				masterManager.initDisplay();
			}
			else {
				console.log(packD1.msg);
			}
		}));

		setman.client.packetManager.addPKey(new PKey("E3", function (iPack) {
			var packE3 = masterManager.PacketE3.decode(iPack.packData);
			if (packE3.success) {
					$('#' + packE3.eID.toString()).remove();
				addEmp('#acceptEmpDiv', toString(packE3.eID));
			}
			else {
				masterManager.setErrorMsg(packE3.msg);
			}
		}));
	}

	this.setHasResumes = function () {
		var params = {
			Bucket: BUCKET_NAME
		}
		masterManager.s3Client.listObjectsV2(params, function (err, data) {
			if (err) {
				masterManager.setErrorMsg("Could not load resumes: " + err.message);
				console.log("Could not load resumes: " + err.message);
			}
			else
			{
				var files = data.Contents.map(function (file) {
					var id = parseInt(file.Key.substr(0, file.Key.indexOf('/')));
					var fileName = file.Key.substr(file.Key.indexOf('/') + 1);
					if (fileName.length > 0) {
						$('#' + id + ' > div > .resumeButton').removeClass('hidden');
						$('#' + id + ' > div > .resumeButton').click({ fKey: file.Key }, function (event) {
							masterManager.viewResume(event.data.fKey);
						});
					}
				});
			}
		});
	}

	this.listHtml = "<li><div class=\"idBox\"><h2></h2><div class=\'resumeButton hidden\'><p>View Resume</p></div><div class=\'acceptButton hidden\'><p>Accept</p></div></div></li>";

	this.addEmp = function (divID, id) {
		$(divID + ' > ul').append(masterManager.listHtml);
		$(divID + ' > ul > li').last().attr('id', id);
		$('#' + id + ' > div > h2').text(id);
		$('#' + id + ' > div > .acceptButton').click({ eID: id }, function (event) {
			masterManager.sendE2(event.data.eID, ACCEPT_ASTATE);
		});
	}

	this.viewResume = function (fKey) {
		var title = fKey.substr(fKey.indexOf('/') + 1);
		masterManager.initPDFDisplay(title);
		masterManager.s3Client.getObject({
			Bucket: BUCKET_NAME,
			Key: fKey
		}, function (err, data) {
			if (err) {
				console.log(err.message);
			}
			else {
				var fileArr = data.Body;
				masterManager.showPDF(title, fileArr);
			}
		});
	}

	this.showPDF = function (name, fileArr) {
		masterManager.pdfIndex = 1;
		PDFJS.getDocument(fileArr).then(function (pdf) {
			masterManager.pdf = pdf;
			masterManager.loadPDFPage = function (page) {
				var scale = 1.5;
				var viewport = page.getViewport(scale);

				var canvas = document.createElement('canvas');
				canvas.style.display = 'block';
				var context = canvas.getContext('2d');
				canvas.height = viewport.height;
				canvas.width = viewport.width;
				canvas.className = 'pdfPage';

				var renderContext = {
					canvasContext: context,
					viewport: viewport
				};
				page.render(renderContext);

				$('#pdfDiv').append(canvas);
				$('#pdfDiv').css('width', String(canvas.width));
				$('#pdfHeadDiv').css('width', String(canvas.width));

				masterManager.pdfIndex++;
				if (masterManager.pdfIndex <= masterManager.pdf.numPages) {
					pdf.getPage(masterManager.pdfIndex).then(masterManager.loadPDFPage);
				}
				else {
					$('#loader').addClass('hidden');
				}
			}
			pdf.getPage(masterManager.pdfIndex).then(masterManager.loadPDFPage);
		});
	}

	this.initPDFDisplay = function(title) {
		$('#pdfTitle').text(title);
		$('body').css('background-color', 'lightgrey');
		$('#pdfBackDiv').removeClass('hidden');
		$('#loader').removeClass('hidden');
		masterManager.pdf = null;
		masterManager.pdfIndex = 0;
		$('#pdfExit').click(function () {
			$('body').css('background-color', 'white');
			$('#pdfBackDiv').addClass('hidden');
			$('.pdfPage').remove();
			$('#pdfExit').unbind('click');
		});
	}

	this.sendE0 = function () {
		var packE0 = masterManager.PacketE0.create({});
		setman.client.tcpConnection.sendPack(new OPacket("E0", true, [0], packE0, masterManager.PacketE0));
	}

	this.sendD2 = function () {
		var packD2 = masterManager.PacketD2.create({});
		setman.client.tcpConnection.sendPack(new OPacket("D2", true, [0], packD2, masterManager.PacketD2));
	}

	this.sendE2 = function (id, nAState) {
		var packE2 = masterManager.PacketE2.create({
			eID: parseInt(id),
			aState: nAState
		});
		setman.client.tcpConnection.sendPack(new OPacket("E2", true, [0], packE2, masterManager.PacketE2));
	}
}
