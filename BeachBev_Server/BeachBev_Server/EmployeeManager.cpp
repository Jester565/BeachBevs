#include "EmployeeManager.h"
#include "Packets/BBPacks.pb.h"
#include "BB_Server.h"
#include "DBManager.h"
#include "BB_Client.h"
#include "CryptoManager.h"
#include "EmailManager.h"
#include "ResumeManager.h"
#include "DebugManager.h"
#include "MasterManager.h"
#include "AcceptManager.h"
#include <WSS_TCPConnection.h>
#include <WSOPacket.h>
#include <ClientManager.h>
#include <PKey.h>
#include <time.h>
#include <thread>
#include <csignal>
#include <aws/email/model/SendEmailRequest.h>

bool EmployeeManager::CheckInTimeRange(OTL_BIGINT& time, int numHours) {
	OTL_BIGINT now = std::time(NULL);
	return (now - time <= numHours * 60 * 60);
}

void EmployeeManager::CreateAccountEmailHandler(const Aws::SES::SESClient * client,
	const Aws::SES::Model::SendEmailRequest & request,
	const Aws::SES::Model::SendEmailOutcome & outcome,
	const AwsSharedPtr<const Aws::Client::AsyncCallerContext>& context)
{
	auto createAccountContext = std::static_pointer_cast<const CreateAccountEmailContext>(context);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(bbServer->getClientManager()->getClient(createAccountContext->clientID));
	if (!outcome.IsSuccess())
	{
		std::string query = "DELETE FROM Employees WHERE eID=:f1<int>";
		try {
			otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *createAccountContext->dbManager->getConnection());
			otlStream << (int)createAccountContext->eID;
		}
		catch (otl_exception ex)
		{
			std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
		}
	}
	ProtobufPackets::PackA1 replyPacket;
	if (outcome.IsSuccess()) {
		if (emailManager->setUnverifiedEmail(createAccountContext->eID, request.GetDestination().GetToAddresses().front(),
			createAccountContext->hashedEmailToken, createAccountContext->dbManager))
		{
			loginClient(sender, createAccountContext->eID);
			replyPacket.set_pwdtoken(createAccountContext->urlEncodedPwdToken);
			replyPacket.set_deviceid(createAccountContext->deviceID);
			replyPacket.set_eid(createAccountContext->eID);
			replyPacket.set_msg("Account Added");
		}
		else
		{
			replyPacket.set_msg("Could not set unverifiedEmail after verification email");
		}
	}
	else
	{
		replyPacket.set_msg("Failed to send verification email: " + AwsErrorToStr(outcome.GetError()));
	}
	if (sender != nullptr) {
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A1");
		oPack->setSenderID(0);
		oPack->addSendToID(sender->getID());
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

void EmployeeManager::PwdResetEmailHandler(const Aws::SES::SESClient * client, const Aws::SES::Model::SendEmailRequest & request, const Aws::SES::Model::SendEmailOutcome & outcome, const AwsSharedPtr<const Aws::Client::AsyncCallerContext>& context)
{
	auto pwdResetContext = std::static_pointer_cast<const PasswordResetContext>(context);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(bbServer->getClientManager()->getClient(pwdResetContext->clientID));
	if (sender != nullptr) {
		ProtobufPackets::PackA5 replyPacket;
		if (outcome.IsSuccess()) {
			replyPacket.set_success(true);
			replyPacket.set_msg("Password reset email sent");
		}
		else
		{
			replyPacket.set_success(false);
			replyPacket.set_msg("Failed to send email");
		}
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A5");
		oPack->setSenderID(0);
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

EmployeeManager::EmployeeManager(BB_Server* bbServer)
	:PKeyOwner(), bbServer(bbServer)
{
	addKey(boost::make_shared<PKey>("A0", this, &EmployeeManager::handleA0));
	addKey(boost::make_shared<PKey>("A2", this, &EmployeeManager::handleA2));
	addKey(boost::make_shared<PKey>("A3", this, &EmployeeManager::handleA3));
	addKey(boost::make_shared<PKey>("A4", this, &EmployeeManager::handleA4));
	addKey(boost::make_shared<PKey>("A6", this, &EmployeeManager::handleA6));
	addKey(boost::make_shared<PKey>("A8", this, &EmployeeManager::handleA8));
	addKey(boost::make_shared<PKey>("C0", this, &EmployeeManager::handleC0));
	addKey(boost::make_shared<PKey>("C2", this, &EmployeeManager::handleC2));
	masterManager = new MasterManager(bbServer);
	emailManager = new EmailManager(bbServer, this);
	resumeManager = new ResumeManager(bbServer, emailManager, masterManager);
	acceptManager = new AcceptManager(bbServer, masterManager, emailManager);
}

void EmployeeManager::follow(ClientPtr client)
{
	PKeyOwner::follow(client);
	acceptManager->follow(client);
	emailManager->follow(client);
	resumeManager->follow(client);
}

void EmployeeManager::handleA0(boost::shared_ptr<IPacket> iPack)
{
	bool success = false;
	ProtobufPackets::PackA0 packA0;
	packA0.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA1 replyPacket;
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	IDType eID = nameToEID(packA0.name(), dbManager);
	if (eID <= 0) {
		eID = emailManager->emailToEID(packA0.name(), dbManager);
		if (eID <= 0) {
			eID = emailManager->emailToEID(packA0.email(), dbManager);
			if (eID <= 0) {
				eID = addEmployeeToDatabase(packA0.name(), dbManager);
				setPwd(eID, packA0.pwd(), dbManager);
				std::string urlEncodedPwdToken;
				DeviceID devID = addPwdToken(eID, urlEncodedPwdToken, dbManager);

				BYTE genToken[TOKEN_SIZE];
				CryptoManager::GenerateRandomData(genToken, TOKEN_SIZE);
				BYTE* genTokenHash = new BYTE[TOKEN_SIZE];
				CryptoManager::GenerateHash(genTokenHash, TOKEN_SIZE, genToken, TOKEN_SIZE);
				std::string urlEncodedEmailToken;
				CryptoManager::UrlEncode(urlEncodedEmailToken, genToken, TOKEN_SIZE);

				AwsSharedPtr<CreateAccountEmailContext> createAccountContext = std::make_shared<CreateAccountEmailContext>();
				createAccountContext->clientID = sender->getID();
				createAccountContext->eID = eID;
				createAccountContext->deviceID = devID;
				createAccountContext->hashedEmailToken = genTokenHash;
				createAccountContext->dbManager = dbManager;
				createAccountContext->urlEncodedPwdToken = urlEncodedPwdToken;
				emailManager->sendVerificationEmail(packA0.email(), urlEncodedEmailToken,
					std::bind(&EmployeeManager::CreateAccountEmailHandler, this, std::placeholders::_1,
						std::placeholders::_2, std::placeholders::_3, std::placeholders::_4),
					createAccountContext);
				success = true;
			}
			else
			{
				replyPacket.set_msg("Email already used");
			}
		}
		else
		{
			replyPacket.set_msg("Name cannot be a used email");
		}
	}
	else
	{
		replyPacket.set_msg("Name already used");
	}
	if (!success) {
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A1");
		oPack->setSenderID(0);
		oPack->addSendToID(sender->getID());
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

void EmployeeManager::handleA2(boost::shared_ptr<IPacket> iPack)
{
	ProtobufPackets::PackA2 packA2;
	packA2.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA9 replyPacket;
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	BYTE dbTokenHash[TOKEN_SIZE];
	OTL_BIGINT tokenTime;
	if (getPwdToken(packA2.eid(), dbTokenHash, tokenTime, packA2.deviceid(), dbManager)) {
		if (CheckInTimeRange(tokenTime, MAX_TOKEN_HOURS)) {
			std::vector<BYTE> packToken;
			packToken.reserve(TOKEN_SIZE);
			CryptoManager::UrlDecode(packToken, packA2.pwdtoken());
			BYTE packTokenHash[TOKEN_SIZE];
			CryptoManager::GenerateHash(packTokenHash, TOKEN_SIZE, packToken.data(), packToken.size());
			bool match = true;
			for (int i = 0; i < TOKEN_SIZE; i++)//Iterate through all to prevent time-based attacks
			{
				if (packTokenHash[i] != dbTokenHash[i]) {
					match = false;
				}
			}
			if (match) {
				std::string urlEncodedPwdToken;
				setPwdToken(packA2.eid(), urlEncodedPwdToken, packA2.deviceid(), dbManager);
				replyPacket.set_pwdtoken(urlEncodedPwdToken);
				replyPacket.set_eid(packA2.eid());
				replyPacket.set_deviceid(packA2.deviceid());
				replyPacket.set_msg("Login successful");
				loginClient(sender, packA2.eid());
			}
			else
			{
				replyPacket.set_msg("Tokens did not match");
			}
		}
		else
		{
			replyPacket.set_msg("Token expired");
		}
	}
	else
	{
		replyPacket.set_msg("Could not aquire a token");
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A9");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

void EmployeeManager::handleA3(boost::shared_ptr<IPacket> iPack)
{
	ProtobufPackets::PackA3 packA3;
	packA3.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA1 replyPacket;
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	IDType eID = nameToEID(packA3.name(), dbManager);
	if (eID <= 0) {
		eID = emailManager->verifiedEmailToEID(packA3.name(), dbManager);
		if (eID <= 0) {
			eID = emailManager->unverifiedEmailToEID(packA3.name(), dbManager);
			std::string verifiedEmail;
			if (emailManager->getVerifiedEmail(eID, verifiedEmail, dbManager)) {
				eID = 0;
			}
		}
	}
	if (eID != 0) {
		BYTE dbPwdHash[HASH_SIZE];
		BYTE dbPwdSalt[SALT_SIZE];
		if (getPwdData(eID, dbPwdHash, dbPwdSalt, dbManager)) {
			BYTE packPwdHash[HASH_SIZE];
			CryptoManager::GenerateHash(packPwdHash, HASH_SIZE,
				(BYTE*)packA3.pwd().data(), packA3.pwd().size(),
				dbPwdSalt, SALT_SIZE);
			bool match = true;
			for (int i = 0; i < HASH_SIZE; i++) {
				if (packPwdHash[i] != dbPwdHash[i]) {
					match = false;
				}
			}
			if (match) {
				std::string urlEncodedPwdToken;
				DeviceID devID = packA3.deviceid();
				if (devID != 0) {
					setPwdToken(eID, urlEncodedPwdToken, packA3.deviceid(), dbManager);
				}
				else {
					devID = addPwdToken(eID, urlEncodedPwdToken, dbManager);
				}
				replyPacket.set_pwdtoken(urlEncodedPwdToken);
				replyPacket.set_eid(eID);
				replyPacket.set_deviceid(devID);
				replyPacket.set_msg("Login successful");
				loginClient(sender, eID);
			}
			else
			{
				replyPacket.set_msg("Invalid login");
			}
		}
		else
		{
			replyPacket.set_msg("Could not get pwd data from database");
		}
	}
	else
	{
		replyPacket.set_msg("Invalid login");
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A1");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

void EmployeeManager::handleA4(boost::shared_ptr<IPacket> iPack)
{
	ProtobufPackets::PackA4 packA4;
	packA4.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA5 replyPacket;
	replyPacket.set_success(false);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	IDType eID = 0;
	IDType unverifiedEID = emailManager->unverifiedEmailToEID(packA4.email(), dbManager);
	if (unverifiedEID > 0) {
		std::string verifiedEmail;
		if (!emailManager->getVerifiedEmail(unverifiedEID, verifiedEmail, dbManager)) {
			eID = unverifiedEID;
		}
		else
		{
			replyPacket.set_msg("Must used verified email");
		}
	}
	else
	{
		eID = emailManager->verifiedEmailToEID(packA4.email(), dbManager);
		if (eID <= 0) {
			replyPacket.set_msg("Email not found");
		}
	}
	if (eID > 0) {
		std::string urlEncodedPwdResetToken;
		if (setPwdResetToken(eID, urlEncodedPwdResetToken, dbManager))
		{
			AwsSharedPtr<PasswordResetContext> pwdResetContext = std::make_shared<PasswordResetContext>();
			pwdResetContext->clientID = iPack->getSenderID();
			emailManager->sendPwdResetEmail(packA4.email(), urlEncodedPwdResetToken,
				std::bind(&EmployeeManager::PwdResetEmailHandler, this, std::placeholders::_1,
					std::placeholders::_2, std::placeholders::_3, std::placeholders::_4),
				pwdResetContext);
			replyPacket.set_success(true);
		}
		else
		{
			replyPacket.set_msg("Failed to set pwdResetToken");
		}
	}
	if (!replyPacket.success()) {
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A5");
		oPack->setSenderID(0);
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

void EmployeeManager::handleA6(boost::shared_ptr<IPacket> iPack)
{
	ProtobufPackets::PackA6 packA6;
	packA6.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA7 replyPacket;
	replyPacket.set_success(false);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	OTL_BIGINT tokenTime;
	IDType eID = 0;
	if (checkPwdResetToken(packA6.pwdresettoken(), eID, tokenTime, dbManager)) {
		if (CheckInTimeRange(tokenTime, MAX_TOKEN_HOURS)) {
			replyPacket.set_success(true);
			replyPacket.set_msg("Valid token");
		}
		else
		{
			replyPacket.set_msg("Token expired");
		}
	}
	else
	{
		replyPacket.set_msg("Invalid token");
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A7");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

void EmployeeManager::handleA8(boost::shared_ptr<IPacket> iPack)
{
	ProtobufPackets::PackA8 packA8;
	packA8.ParseFromString(*iPack->getData());
	ProtobufPackets::PackA1 replyPacket;
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	OTL_BIGINT tokenTime;
	IDType eID;
	if (checkPwdResetToken(packA8.pwdresettoken(), eID, tokenTime, dbManager)) {
		if (CheckInTimeRange(tokenTime, MAX_TOKEN_HOURS)) {
			if (setPwd(eID, packA8.pwd(), dbManager)) {
				std::string urlEncodedPwdToken;
				DeviceID devID = addPwdToken(eID, urlEncodedPwdToken, dbManager);
				replyPacket.set_pwdtoken(urlEncodedPwdToken);
				replyPacket.set_eid(eID);
				replyPacket.set_deviceid(devID);
				replyPacket.set_msg("Successful");
				removePwdResetToken(eID, dbManager);
			}
			else
			{
				replyPacket.set_msg("Could not set password");
			}
		}
		else
		{
			replyPacket.set_msg("Token expired");
		}
	}
	else
	{
		replyPacket.set_msg("Invalid token");
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("A1");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

void EmployeeManager::handleC0(boost::shared_ptr<IPacket> iPack)
{
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	ProtobufPackets::PackC1 replyPacket;
	replyPacket.set_name("ERROR");
	if (masterManager->isMaster(sender->getEmpID(), dbManager)) {
		ProtobufPackets::PackC0 packC0;
		packC0.ParseFromString(*iPack->getData());
		std::string name;
		if (eIDToName(packC0.eid(), dbManager, name))
		{
			replyPacket.set_name(name);
		}
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("C1");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

void EmployeeManager::handleC2(boost::shared_ptr<IPacket> iPack)
{
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	ProtobufPackets::PackC3 replyPacket;
	replyPacket.set_name("ERROR");
	std::string name;
	if (eIDToName(sender->getEmpID(), dbManager, name)) {
		replyPacket.set_name(name);
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("C3");
	oPack->setSenderID(0);
	oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}

BB_ClientPtr EmployeeManager::getEmployee(IDType eID)
{
	auto it = employees.find(eID);
	if (it != employees.end())
	{
		return boost::static_pointer_cast<BB_Client>(it->second);
	}
	return nullptr;
}

IDType EmployeeManager::addEmployeeToDatabase(const std::string & name, DBManager * dbManager)
{
	IDType eID = getNextEID(dbManager);
	std::string query = "INSERT INTO Employees (eID, name) VALUES (:f1<int>, :f2<char[";
	query += std::to_string(NAME_SIZE);
	query += "]>)";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		otlStream << name;
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return eID;
}

bool EmployeeManager::setPwd(IDType eID, const std::string & pwd, DBManager * dbManager)
{
	clearPwdTokens(eID, dbManager);
	BYTE genSalt[SALT_SIZE];
	CryptoManager::GenerateRandomData(genSalt, SALT_SIZE);
	BYTE genHash[HASH_SIZE];
	CryptoManager::GenerateHash(genHash, HASH_SIZE, (BYTE*)pwd.data(), pwd.size(), genSalt, SALT_SIZE);

	std::string query = "UPDATE Employees SET pwdHash=:f1<raw[";
	query += std::to_string(HASH_SIZE);
	query += "]>, pwdSalt=:f2<raw[";
	query += std::to_string(SALT_SIZE);
	query += "]> WHERE eID=:f3<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		CryptoManager::OutputBytes(otlStream, genHash, HASH_SIZE);
		CryptoManager::OutputBytes(otlStream, genSalt, SALT_SIZE);
		otlStream << (int)eID;
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
		return false;
	}
	return true;
}

bool EmployeeManager::setPwdToken(IDType eID, std::string& urlEncodedPwdToken, DeviceID devID, DBManager * dbManager)
{
	BYTE genToken[TOKEN_SIZE];
	CryptoManager::GenerateRandomData(genToken, TOKEN_SIZE);
	BYTE genTokenHash[TOKEN_SIZE];
	CryptoManager::GenerateHash(genTokenHash, TOKEN_SIZE, genToken, TOKEN_SIZE);
	std::string query = "REPLACE INTO PwdTokens VALUES (:f1<int>, :f2<int>, :f3<raw[";
	query += std::to_string(TOKEN_SIZE);
	query += "]>, :f4<bigint>)";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		otlStream << (int)devID;
		CryptoManager::OutputBytes(otlStream, genTokenHash, TOKEN_SIZE);
		otlStream << (OTL_BIGINT)(std::time(NULL));
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	CryptoManager::UrlEncode(urlEncodedPwdToken, genToken, TOKEN_SIZE);
	return true;
}

bool EmployeeManager::setPwdResetToken(IDType eID, std::string & urlEncodedPwdResetToken, DBManager * dbManager)
{
	BYTE genToken[TOKEN_SIZE];
	CryptoManager::GenerateRandomData(genToken, TOKEN_SIZE);
	BYTE genTokenHash[TOKEN_SIZE];
	CryptoManager::GenerateHash(genTokenHash, TOKEN_SIZE, genToken, TOKEN_SIZE);
	std::string query = "REPLACE INTO PwdResetTokens VALUES (:f1<int>, :f2<raw[";
	query += std::to_string(TOKEN_SIZE);
	query += "]>, :f3<bigint>)";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		CryptoManager::OutputBytes(otlStream, genTokenHash, TOKEN_SIZE);
		otlStream << (OTL_BIGINT)(std::time(NULL));
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	CryptoManager::UrlEncode(urlEncodedPwdResetToken, genToken, TOKEN_SIZE);
	return true;
}

bool EmployeeManager::clearPwdTokens(IDType eID, DBManager * dbManager)
{
	std::string query = "DELETE FROM PwdTokens WHERE eID=:f1<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
}

DeviceID EmployeeManager::addPwdToken(IDType eID, std::string & urlEncodedPwdToken, DBManager * dbManager)
{
	DeviceID devID = getNextDeviceID(eID, dbManager);
	setPwdToken(eID, urlEncodedPwdToken, devID, dbManager);
	return devID;
}

DeviceID EmployeeManager::getNextDeviceID(IDType eID, DBManager * dbManager)
{
	DeviceID devID = 0;
	std::string query = "SELECT deviceID FROM PwdTokens WHERE eID=:f1<int> ORDER BY deviceID desc limit 1";
	try
	{
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		if (!otlStream.eof())
		{
			int devInt = 0;
			otlStream >> devInt;
			devID = devInt;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return devID + 1;
}

IDType EmployeeManager::nameToEID(const std::string& name, DBManager* dbManager)
{
	IDType eID = 0;
	std::string query = "SELECT eID FROM Employees WHERE name=:f1<char[";
	query += std::to_string((int)NAME_SIZE);
	query += "]>";
	try
	{
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << name;
		if (!otlStream.eof()) {
			int eIDInt = 0;
			otlStream >> eIDInt;
			eID = eIDInt;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return eID;
}

IDType EmployeeManager::getNextEID(DBManager* dbManager)
{
	IDType eID = 0;
	std::string query = "SELECT * FROM Employees ORDER BY eID desc limit 1";
	try
	{
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		dbManager->getConnection()->commit();
		if (!otlStream.eof())
		{
			int eIDInt = 0;
			otlStream >> eIDInt;
			eID = eIDInt;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return eID + 1;
}

bool EmployeeManager::getPwdData(IDType eID, BYTE * hash, BYTE * salt, DBManager * dbManager)
{
	std::string query = "SELECT pwdHash, pwdSalt FROM Employees WHERE eID = :f1<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		if (!otlStream.eof()) {
			CryptoManager::InputBytes(otlStream, hash, HASH_SIZE);
			CryptoManager::InputBytes(otlStream, salt, SALT_SIZE);
			return true;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return false;
}

bool EmployeeManager::getPwdToken(IDType eID, BYTE * databaseTokenHash, OTL_BIGINT& tokenTime, DeviceID devID, DBManager * dbManager)
{
	std::string query = "SELECT tokenHash, tokenTime FROM PwdTokens WHERE eID = :f1<int> AND deviceID = :f2<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		otlStream << (int)devID;
		if (!otlStream.eof()) {
			CryptoManager::InputBytes(otlStream, databaseTokenHash, TOKEN_SIZE);
			otlStream >> tokenTime;
			return true;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return false;
}

bool EmployeeManager::checkPwdResetToken(const std::string& urlEncodedPwdResetToken, IDType& eID, OTL_BIGINT& tokenTime, DBManager * dbManager)
{
	std::vector <BYTE> pwdResetToken;
	pwdResetToken.reserve(TOKEN_SIZE);
	CryptoManager::UrlDecode(pwdResetToken, urlEncodedPwdResetToken);
	BYTE pwdResetTokenHash[TOKEN_SIZE];
	CryptoManager::GenerateHash(pwdResetTokenHash, TOKEN_SIZE, pwdResetToken.data(), pwdResetToken.size());
	std::string query = "SELECT eID, tokenTime FROM PwdResetTokens WHERE tokenHash=:f1<raw[";
	query += std::to_string(TOKEN_SIZE);
	query += "]>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		CryptoManager::OutputBytes(otlStream, pwdResetTokenHash, TOKEN_SIZE);
		if (!otlStream.eof()) {
			int eIDInt = 0;
			otlStream >> eIDInt;
			eID = (IDType)eIDInt;
			otlStream >> tokenTime;
			return true;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return false;
}

bool EmployeeManager::removePwdResetToken(IDType eID, DBManager * dbManager)
{
	std::string query = "DELETE FROM PwdResetTokens WHERE eID=:f1<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		return true;
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
	return false;
}

void EmployeeManager::loginClient(BB_ClientPtr bbClient, IDType eID)
{
	bbClient->setEmpID(eID);
	employees.emplace(std::make_pair(eID, bbClient));
}

bool EmployeeManager::eIDToName(IDType eID, DBManager * dbManager, std::string & name)
{
	std::string query = "SELECT name FROM Employees WHERE eID=:f1<int>";
	try {
		otl_stream otlStream(OTL_BUFFER_SIZE, query.c_str(), *dbManager->getConnection());
		otlStream << (int)eID;
		if (!otlStream.eof()) {
			otlStream >> name;
			return true;
		}
	}
	catch (otl_exception ex)
	{
		std::cerr << "Code: " << ex.code << std::endl << "MSG: " << ex.msg << std::endl;
	}
}

EmployeeManager::~EmployeeManager()
{
}
