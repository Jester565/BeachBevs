#include "ResumeManager.h"
#include "Packets/BBPacks.pb.h"
#include "BB_Server.h"
#include "BB_Client.h"
#include "EmailManager.h"
#include "MasterManager.h"
#include <ClientManager.h>
#include <WSOPacket.h>
#include <WSIPacket.h>
#include <boost/make_shared.hpp>
#include <aws/sts/model/GetFederationTokenRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>

const std::string ResumeManager::USER_RESUME_POLICY_PT1 = "{\
\"Version\": \"2012-10-17\",\
\"Statement\" : [\
{\
	\"Sid\": \"AllowListingOfUserFolder\",\
		\"Effect\" : \"Allow\",\
		\"Action\" : [\
			\"s3:ListBucket\"\
		],\
		\"Resource\" : [\
			\"arn:";
const std::string ResumeManager::USER_RESUME_POLICY_PT2 = "\"\
	],\
	\"Condition\" : {\"StringLike\":{\"s3:prefix\":[\"";
const std::string ResumeManager::USER_RESUME_POLICY_PT3 = "/*\"]}}\
},\
{\
	\"Sid\": \"Stmt1487575487000\",\
	\"Effect\" : \"Allow\",\
	\"Action\" : [\
		\"s3:GetObject\",\
			\"s3:PutObject\"\
	],\
	\"Resource\" : [\
		\"arn:";

const std::string ResumeManager::USER_RESUME_POLICY_PT4 = "/*\"\
	]\
}\
]\
}";

const std::string ResumeManager::MASTER_RESUME_POLICY_PT1 = "{\
\"Version\": \"2012-10-17\",\
\"Statement\" : [\
{\
	\"Effect\": \"Allow\",\
		\"Action\" : [\
			\"s3:ListBucket\"\
		],\
		\"Resource\" : [\
			\"arn:";
const std::string ResumeManager::MASTER_RESUME_POLICY_PT2 = "\"\
		]\
},\
{\
	\"Sid\": \"Stmt1487575487000\",\
	\"Effect\" : \"Allow\",\
	\"Action\" : [\
		\"s3:GetObject\",\
		\"s3:PutObject\"\
	],\
	\"Resource\" : [\
		\"arn:";
const std::string ResumeManager::MASTER_RESUME_POLICY_PT3 = "/*\"\
	]\
}\
]\
}";

const std::string ResumeManager::RESUME_BUCKET_NAME = "beachbev-resumes";

const std::string ResumeManager::RESUME_BUCKET_ARN = "aws:s3:::beachbev-resumes";

const std::string ResumeManager::IAM_USER_NAME = "pdf_usr";

ResumeManager::ResumeManager(BB_Server* bbServer, EmailManager* emailManager, MasterManager* masterManager)
	:PKeyOwner(), bbServer(bbServer), emailManager(emailManager), masterManager(masterManager)
{
	if (!initStsClient()) {
		std::cerr << "Could not initialize STS client!" << std::endl;
	}
	if (!initS3Client()) {
		std::cerr << "Could not initialize S3 client!" << std::endl;
	}
	addKey(boost::make_shared<PKey>("D0", this, &ResumeManager::handleD0));
	addKey(boost::make_shared<PKey>("D2", this, &ResumeManager::handleD2));
	addKey(boost::make_shared<PKey>("D3", this, &ResumeManager::handleD3));
}

bool ResumeManager::initStsClient()
{
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.region = AWS_SERVER_REGION_1;
	stsClient = Aws::MakeShared<Aws::STS::STSClient>(AWS_ALLOC_TAG, clientConfig);
	return true;
}

bool ResumeManager::initS3Client()
{
	Aws::Client::ClientConfiguration clientConfig;
	clientConfig.region = AWS_SERVER_REGION_1;
	s3Client = Aws::MakeShared<Aws::S3::S3Client>(AWS_ALLOC_TAG, clientConfig);
	return true;
}

void ResumeManager::handleD0(boost::shared_ptr<IPacket> iPack)
{
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	std::string email;
	if (sender->getEmpID() > 0) {
		if (emailManager->getVerifiedEmail(sender->getEmpID(), email, sender->getDBManager())) {
			std::string policy;
			createUserResumePolicy(sender->getEmpID(), policy);
			requestResumePermissions(sender, policy);
		}
		else
		{
			ProtobufPackets::PackD1 replyPacket;
			replyPacket.set_msg("Email is not verified");
			boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("D1");
			oPack->setSenderID(0);
			oPack->addSendToID(sender->getID());
			oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
			bbServer->getClientManager()->send(oPack, sender);
		}
	}
}

void ResumeManager::handleD2(boost::shared_ptr<IPacket> iPack)
{
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	DBManager* dbManager = sender->getDBManager();
	if (masterManager->isMaster(sender->getEmpID(), dbManager)) {
		std::string policy;
		createMasterResumePolicy(policy);
		requestResumePermissions(sender, policy);
	}
	else
	{
		ProtobufPackets::PackD1 replyPacket;
		replyPacket.set_msg("Not a master");
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("D1");
		oPack->setSenderID(0);
		oPack->addSendToID(sender->getID());
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

void ResumeManager::handleD3(boost::shared_ptr<IPacket> iPack)
{
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(iPack->getSender());
	if (sender == nullptr) {
		return;
	}
	AwsSharedPtr<HasResumeContext> hasResumeContext = Aws::MakeShared<HasResumeContext>(AWS_ALLOC_TAG);
	hasResumeContext->clientID = sender->getID();
	Aws::S3::Model::ListObjectsV2Request request;
	request.SetBucket(RESUME_BUCKET_NAME);
	std::string folderPrefix = std::to_string(sender->getEmpID());
	folderPrefix += '/';
	request.SetPrefix(folderPrefix);
	s3Client->ListObjectsV2Async(request, std::bind(&ResumeManager::hasResumeHandler, this, std::placeholders::_1,
		std::placeholders::_2, std::placeholders::_3, std::placeholders::_4), hasResumeContext);
}

ResumeManager::~ResumeManager()
{
}

bool ResumeManager::requestResumePermissions(BB_ClientPtr sender, const std::string& policy)
{
	auto context = Aws::MakeShared<RequestResumePermContext>(AWS_ALLOC_TAG);
	context->clientID = sender->getID();
	context->folderObjKey = std::to_string(sender->getEmpID());
	Aws::STS::Model::GetFederationTokenRequest request;
	request.SetPolicy(policy.c_str());
	request.SetDurationSeconds(USER_RESUME_DURATION);
	request.SetName(IAM_USER_NAME.c_str());
	stsClient->GetFederationTokenAsync(request, std::bind(&ResumeManager::requestResumeHandler, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4), context);
	return true;
}

void ResumeManager::requestResumeHandler(const Aws::STS::STSClient * stsClient, const Aws::STS::Model::GetFederationTokenRequest & req, const Aws::STS::Model::GetFederationTokenOutcome & outcome, const AwsSharedPtr<const Aws::Client::AsyncCallerContext>& context)
{
	auto resumeContext = std::static_pointer_cast<const RequestResumePermContext>(context);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(bbServer->getClientManager()->getClient(resumeContext->clientID));
	if (sender != nullptr) {
		ProtobufPackets::PackD1 replyPacket;
		if (outcome.IsSuccess()) {
			auto credentials = outcome.GetResult().GetCredentials();
			replyPacket.set_folderobjkey(resumeContext->folderObjKey);
			replyPacket.set_accesskeyid(AwsStrToStr(credentials.GetAccessKeyId()));
			replyPacket.set_accesskey(AwsStrToStr(credentials.GetSecretAccessKey()));
			replyPacket.set_sessionkey(AwsStrToStr(credentials.GetSessionToken()));
		}
		else
		{
			replyPacket.set_msg("Failed to request access id: " + AwsErrorToStr(outcome.GetError()));
			std::cerr << replyPacket.msg();
		}
		boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("D1");
		oPack->setSenderID(0);
		oPack->addSendToID(sender->getID());
		oPack->setData(boost::make_shared<std::string>(replyPacket.SerializeAsString()));
		bbServer->getClientManager()->send(oPack, sender);
	}
}

void ResumeManager::createUserResumePolicy(IDType eID, std::string & policy)
{
	std::string folderObjKey = std::to_string(eID);
	policy = USER_RESUME_POLICY_PT1;
	policy += RESUME_BUCKET_ARN;
	policy += USER_RESUME_POLICY_PT2;
	policy += folderObjKey;
	policy += USER_RESUME_POLICY_PT3;
	policy += RESUME_BUCKET_ARN;
	policy += "/";
	policy += folderObjKey;
	policy += USER_RESUME_POLICY_PT4;
}

void ResumeManager::createMasterResumePolicy(std::string & policy)
{
	policy += MASTER_RESUME_POLICY_PT1;
	policy += RESUME_BUCKET_ARN;
	policy += MASTER_RESUME_POLICY_PT2;
	policy += RESUME_BUCKET_ARN;
	policy += MASTER_RESUME_POLICY_PT3;
}

void ResumeManager::hasResumeHandler(const Aws::S3::S3Client * s3Client, const Aws::S3::Model::ListObjectsV2Request & req, const Aws::S3::Model::ListObjectsV2Outcome & outcome, const AwsSharedPtr<const Aws::Client::AsyncCallerContext>& context)
{
	auto hasResumeContext = std::static_pointer_cast<const HasResumeContext>(context);
	BB_ClientPtr sender = boost::static_pointer_cast<BB_Client>(bbServer->getClientManager()->getClient(hasResumeContext->clientID));
	if (sender == nullptr) {
		return;
	}
	ProtobufPackets::PackD4 packD4;
	packD4.set_hasresume(false);
	if (outcome.IsSuccess()) {
		packD4.set_hasresume((outcome.GetResult().GetKeyCount() > 0));
	}
	else
	{
		std::cerr << "Error in hasResumeHandler: " << AwsErrorToStr(outcome.GetError()) << std::endl;
	}
	boost::shared_ptr<OPacket> oPack = boost::make_shared<WSOPacket>("D4");
	oPack->setSenderID(0);
	oPack->addSendToID(sender->getID());
	oPack->setData(boost::make_shared<std::string>(packD4.SerializeAsString()));
	bbServer->getClientManager()->send(oPack, sender);
}
