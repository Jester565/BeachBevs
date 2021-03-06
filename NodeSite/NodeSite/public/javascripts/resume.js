﻿'use strict';

var BUCKET_REGION = 'us-west-1';
var BUCKET_NAME = 'beachbev-resumes';

var setman = null;
var resumeManager = null;
var resumeZone = null;

function s3Decode(str) {
	return decodeURIComponent((str + '').replace(/\+/g, '%20'));
}

$('document').ready(function () {
	$("#mBar").load("./mBar.html", function () {
		resumeManager = new ResumeManager();
		setman = new SetupManager(true, resumeManager);
	});
});

function ResumeManager() {
	resumeManager = this;

	this.onProto = function () {
		resumeManager.initPackets();
	}

	this.onOpen = function () {
		resumeManager.sendD0();
	}

	this.s3Client = null;
	this.s3Prefix = null;
	this.pdf = null;
	this.pdfIndex = 0;
	this.pdfW = 0;
	this.pdfData = null;

	this.setErrorMsg = function (msg) {
		$('#msg').text(msg);
		$('#msg').removeClass('hidden');
		$('#msg').focus();
		$('html, body').scrollTo($('#msg'), 100);
	}

	this.clearErrorMsg = function () {
		$('#msg').addClass('hidden');
	}

	this.initDisplay = function () {
		$('#uploadDiv').removeClass('hidden');
		$('#loading').addClass('hidden');
	}

	this.initAWS = function (resumeCreds) {
		AWS.config.update({
			credentials: resumeCreds,
			region: BUCKET_REGION
		});
		resumeManager.s3Client = new AWS.S3({ apiVersion: '2006-03-01' });
	}

	this.initPackets = function (root) {
		resumeManager.PacketD0 = setman.client.root.lookup("ProtobufPackets.PackD0");
		resumeManager.PacketD1 = setman.client.root.lookup("ProtobufPackets.PackD1");
		setman.client.packetManager.addPKey(new PKey("D1", function (iPack) {
			var packD1 = resumeManager.PacketD1.decode(iPack.packData);
			if (packD1.accessKey.length > 0) {
				resumeManager.s3Prefix = packD1.folderObjKey;
				var credentials = new AWS.Credentials(packD1.accessKeyID, packD1.accessKey, packD1.sessionKey);
				resumeManager.initAWS(credentials);
				resumeManager.loadResumes();
			}
			else {
				console.log(packD1.msg);
			}
		}));
	}

	this.sendD0 = function () {
		var packD0 = resumeManager.PacketD0.create({});
		setman.client.tcpConnection.sendPack(new OPacket("D0", true, [0], packD0, resumeManager.PacketD0));
	}

	this.loadResumes = function () {
		var resumeFolderKey = resumeManager.s3Prefix + '/';
		var params = {
			Bucket: BUCKET_NAME,
			Prefix: resumeFolderKey,
			MaxKeys: 100,
			FetchOwner: false,
			EncodingType: 'url',
		}
		resumeManager.s3Client.listObjectsV2(params, function (err, data) {
			if (err) {
				resumeManager.setErrorMsg("Could not load resume folder: " + err.message);
			}
			else {
				var files = data.Contents.map(function (file) {
					var fileName = s3Decode(file.Key.substr(file.Key.indexOf('/') + 1));
					if (fileName.length > 0) {
						file.size = file.Size;
						file.name = fileName;
						file.type = 'application/pdf';
						file.uploaded = true;
						resumeZone.addFile(file);
						resumeZone.emit("complete", file);
					}
				});
			}
			resumeManager.initDisplay();
		})
	}

	this.uploadResume = function (file) {
		var fileKey = resumeManager.s3Prefix + '/' + file.name;
		resumeManager.s3Client.upload({
			Bucket: BUCKET_NAME,
			Key: fileKey,
			ContentType: file.type,
			Body: file
		}, function (err, data) {
			if (err) {
				resumeManager.setErrorMsg(err.message);
				$('#uploadButton h2').text('Failed');
			}
			else {
				file.uploaded = true;
				resumeZone.emit("complete", file);
				resumeZone.emit("success", file);
				$('#uploadButton h2').text('Complete');
			}
		});
	}

	this.viewResume = function (file) {
		resumeManager.initPDFDisplay(file.name);
		if (file.uploaded) {
			var fileKey = resumeManager.s3Prefix + '/' + file.name;
			resumeManager.s3Client.getObject({
				Bucket: BUCKET_NAME,
				Key: fileKey
			}, function (err, data) {
				if (err) {
					console.log(err.message);
				}
				else
				{
					var fileArr = data.Body;
					resumeManager.showPDF(file.name, fileArr);
				}
			});
		}
		else
		{
			var fReader = new FileReader();
			fReader.onload = function () {
				var fileData = new Uint8Array(this.result);
				resumeManager.showPDF(file.name, fileData);
			}
			fReader.readAsArrayBuffer(file);
		}
	}

	this.setPDFSize = function () {
		var w = $(window).width();
		if (resumeManager.pdfW < $(window).width()) {
			w = resumeManager.pdfW;
		}
		$('#pdfDiv').css('width', String(w) + 'px');
		$('#pdfHeadDiv').css('width', String(w) + 'px');
		$('.pdfPage').css('width', String(w - 20) + 'px');
	}

	this.showPDF = function (name, fileArr) {
		resumeManager.pdfIndex = 1;
		resumeManager.pdfData = fileArr;
		PDFJS.getDocument(fileArr).then(function (pdf) {
			resumeManager.pdf = pdf;
			resumeManager.loadPDFPage = function (page) {
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

				resumeManager.pdfW = canvas.width;
				resumeManager.pdfH = canvas.height;
				$('#pdfDiv').append(canvas);
				resumeManager.setPDFSize();

				resumeManager.pdfIndex++;
				if (resumeManager.pdfIndex <= resumeManager.pdf.numPages) {
					pdf.getPage(resumeManager.pdfIndex).then(resumeManager.loadPDFPage);
				}
				else
				{
					$('#loader').addClass('hidden');
				}
			}
			pdf.getPage(resumeManager.pdfIndex).then(resumeManager.loadPDFPage);
		});
	}
	this.initPDFDisplay = function (title) {
		$(window).resize(resumeManager.setPDFSize);
		$('#loader').removeClass('hidden');
		$('#pdfTitle').text(title);
		$('body').css('background-color', 'lightgrey');
		$('#uploadDiv').addClass('hidden');
		$('#pdfBackDiv').removeClass('hidden');
		resumeManager.pdf = null;
		resumeManager.pdfIndex = 0;
		$('#pdfDownload').click(function () {
			var pdfBlob = new Blob([resumeManager.pdfData], { type: "application/pdf" });
			saveAs(pdfBlob, $('#pdfTitle').text());

			var android = navigator.userAgent.toLowerCase().indexOf("android") > -1;
			if (android) {
				alert("Android may say the file did not download correctly. However, you can open it via Chrome Settings->Downloads->" + $('#pdfTitle').text());
			}
		});
		$('#pdfExit').click(function () {
			$('body').css('background-color', 'white');
			$('#pdfBackDiv').addClass('hidden');
			$('.pdfPage').remove();
			$('#pdfExit').unbind('click');
			$('#uploadDiv').removeClass('hidden');
		});
	}
}

Dropzone.options.resumezone = {
	maxFilesize: 2,
	autoProcessQueue: false,
	maxFiles: 1,
	acceptedFiles: "application/pdf",
	thumbnailWidth: 150,
	thumbnailHeight: 190,
	clickable: true,
	url: function (file) {

	},
	init: function () {
		resumeZone = this;

		this.on("addedfile", function (file) {
			resumeManager.clearErrorMsg();
			file.previewElement.querySelector("img").src = "./images/pdfIcon.png";
			file.previewElement.addEventListener("click", function () {
				resumeManager.viewResume(file);
			});
			if (!file.uploaded) {
				$('#uploadButton').addClass('active');
				$('#uploadButton h2').text('Upload');
				$('#uploadButton').click(function () {
					if (resumeZone.files.length > 0) {
						if (!resumeZone.files[0].uploaded) {
							resumeManager.clearErrorMsg();
							$('#uploadButton').removeClass('active');
							$('#uploadButton h2').text('Uploading...');
							$('#uploadButton').unbind('click');
							resumeManager.uploadResume(resumeZone.files[0]);
						}
						else
						{
							resumeManager.setErrorMsg('File is already uploaded');
						}
					}
					else
					{
						resumeManager.setErrorMsg('No files to upload');
					}
				});
			}
		});

		this.on("error", function (file) {
			if (!file.accepted) {
				this.removeFile(file);
				if (file.type !== 'application/pdf') {
					resumeManager.setErrorMsg("File was not a pdf");
				}
				else if (file.size > 2000000) {
					resumeManager.setErrorMsg("File size exceeded 2 Mb");
				}
				else
				{
					resumeManager.setErrorMsg("File not accepted");
				}
			}
		});

		this.on("maxfilesexceeded", function (file) {
			this.removeAllFiles();
			this.addFile(file);
		});
	}
}
