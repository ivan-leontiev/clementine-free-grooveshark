#include "groovesharkclient.h"
#include "core/closure.h"
#include "core/logging.h"

#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QDateTime>
#include <QStringList>
#include <QSignalMapper>
#include <QStateMachine>
#include <QHistoryState>
#include <QSettings>
#include <QNetworkReply>
#include <QTimer>
#include <QCryptographicHash>
#include <QUuid>
#include <qjson/parser.h>
#include <qjson/serializer.h>

const QString kSettingsGroup = "Grooveshark";
const int GSClient::kCTokenTimeout = 600 * 1000;
const int GSReply::kGSReplyTimeout = 20000;
static const QString kGSMoreUrl = "https://grooveshark.com/more.php?%1";
static const QString kGSHomeUrl = "http://grooveshark.com/";

static const ClientPreset kMobileClient = {"mobileshark", 20120830,
                                           "gooeyFlubber"};

static const ClientPreset kJSClient = {"jsqueue", 20130520, "nuggetsOfBaller"};

static const ClientPreset kHtmlClient = {"htmlshark", 20130520, "nuggetsOfBaller"};

GSClient::GSClient(QObject* parent)
    : QObject(parent),
      network_(new QNetworkAccessManager(this)),
      sm_(new QStateMachine(this)),
      ctoken_timer_(new QTimer) {
  QSettings s;
  s.beginGroup(kSettingsGroup);

  session_ = s.value("sessionid").toString();
  user_id_ = s.value("userID").toString();
  uuid_ = QUuid::createUuid().toString().mid(1, 36).toUpper();

  network_->setCookieJar(new QNetworkCookieJar());
  SetLoggedIn(!user_id_.isEmpty());

  ctoken_timer_->setSingleShot(true);
  ctoken_timer_->setInterval(kCTokenTimeout);
  connect(this, SIGNAL(Fault()), this, SLOT(OnFault()));

  SetupSM();
}

void GSClient::CreateSession() {
  qLog(Debug) << Q_FUNC_INFO;

  GSReply* gsreply =
      Request("initiateSession", QList<Param>(), false, SysRequestEventType);

  NewClosure(gsreply, SIGNAL(Finished()), this, SLOT(SessionCreated(GSReply*)),
             gsreply);
}

void GSClient::SessionCreated(GSReply* reply) {
  reply->deleteLater();

  if (reply->hasError()) {
    qLog(Error) << "Failed to create Grooveshark session.";
    emit Fault();
    return;
  }

  QString result = reply->getResult().toString();
  session_ = result;

  RetrieveGSConfig();
}

void GSClient::RetrieveGSConfig() {
  qLog(Debug) << Q_FUNC_INFO;

  GSReply* gsreply = Request("getGSConfig", QList<Param>(), false, SysRequestEventType);
  NewClosure(gsreply, SIGNAL(Finished()), this,
             SLOT(GSConfigRetrieved(GSReply*)), gsreply);
}

void GSClient::GSConfigRetrieved(GSReply* reply) {
  reply->deleteLater();

  if (reply->hasError()) {
    qLog(Error) << "Error while retrieving GSConfig: " << reply->getErrorMsg();
    emit Fault();
    return;
  }

  QVariantMap result = reply->getResult().toMap();

  // uuid_ = result["getGSConfig"].toMap()["uuid"].toString();
  country_ = result["country"].toMap();

  emit Ok();
}

void GSClient::UpdateCommunicationToken() {
  qLog(Debug) << Q_FUNC_INFO;

  QList<Param> params;
  params << Param("secretKey", QString(QCryptographicHash::hash(
                                           session_.toUtf8(),
                                           QCryptographicHash::Md5).toHex()));

  GSReply* gsreply =
      Request("getCommunicationToken", params, false, SysRequestEventType);
  NewClosure(gsreply, SIGNAL(Finished()), this,
             SLOT(CommunicationTokenUpdated(GSReply*)), gsreply);
}

void GSClient::CommunicationTokenUpdated(GSReply* reply) {
  reply->deleteLater();

  if (reply->hasError()) {
    qLog(Error) << "Error while updating communication token.";
    emit Fault();
    return;
  }

  QString result = reply->getResult().toString();
  ctoken_ = result;
  ctoken_timer_->start();
  emit Ok();
}

void GSClient::AuthenticateAsAuthorizedUser() {
  qLog(Debug) << Q_FUNC_INFO;
  if (user_id_.isEmpty()) {
    emit LoginFinished(false);
    return;
  }

  GSReply* reply = Request("authenticateAsAuthorizedUser",
                           QList<Param>() << Param("userID", user_id_), false);
  NewClosure(reply, SIGNAL(Finished()), this, SLOT(LoggedIn(GSReply*)), reply);
}

void GSClient::SetupSM() {
  QState* s11 = new QState(); // idle
  QState* s12 = new QState(); // ctoken expired

  QState* s2 = new QState();     // connecting group
  QState* s21 = new QState(s2);  // creating session
  QState* s22 = new QState(s2);  // updating ctoken

  QState* s3 = new QState();  // connected group
  QHistoryState* s3h = new QHistoryState(QHistoryState::DeepHistory,
                                         s3);  // connected state history

  QState* s31 = new QState(s3);  // authenticating

  QState* s32 = new QState(s3);    // ready subgroup
  QState* s321 = new QState(s32);  // not logged in
  QState* s322 = new QState(s32);  // logged in

  // idle
  DeferRequestTransition* s11_to_s2 = new DeferRequestTransition(this);
  s11_to_s2->setTargetState(s2);
  s11->addTransition(s11_to_s2);

  // ctoken expired
  DeferRequestTransition* s12_to_s2 = new DeferRequestTransition(this);
  s12_to_s2->setTargetState(s22);
  s12->addTransition(s12_to_s2);

  //  connecting group
  s2->setInitialState(s21);
  s2->addTransition(this, SIGNAL(Fault()), s11);

  connect(s21, SIGNAL(entered()), this, SLOT(CreateSession()));
  s21->addTransition(this, SIGNAL(Ok()), s22);
  connect(s22, SIGNAL(entered()), this, SLOT(UpdateCommunicationToken()));
  s22->addTransition(this, SIGNAL(Ok()), s3);

  DeferRequestTransition* s2_to_s2 = new DeferRequestTransition(this);
  s2->addTransition(s2_to_s2);

  //  connected group
  s3->setInitialState(s3h);
  s3h->setDefaultState(s31);
  s3->addTransition(ctoken_timer_, SIGNAL(timeout()), s12);
  s3->addTransition(this, SIGNAL(CTExpired()), s12);
  s3->addTransition(this, SIGNAL(SessionExpired()), s11);

  RequestTransition* s3_req_nauth = new RequestTransition(this, false);
  s3->addTransition(s3_req_nauth);

  //  authenticating
  connect(s31, SIGNAL(entered()), this, SLOT(AuthenticateAsAuthorizedUser()));

  RequestTransition* s31_req_auth = new DeferRequestTransition(this, true);
  s31->addTransition(s31_req_auth);

  LoginFinishedTransition* s31_login_done =
      new LoginFinishedTransition(this, true, s31);
  s31_login_done->setTargetState(s322);

  LoginFinishedTransition* s31_login_fault =
      new LoginFinishedTransition(this, false, s31);
  s31_login_fault->setTargetState(s321);

  //  ready subgroup
  connect(s32, SIGNAL(entered()), this, SIGNAL(Ready()));

  CancelRequestTransition* s321_req_auth =
      new CancelRequestTransition(this, true);
  s321->addTransition(s321_req_auth);

  LoginFinishedTransition* s321_login_done =
      new LoginFinishedTransition(this, true, s321);
  s321_login_done->setTargetState(s322);

  RequestTransition* s322_req_auth = new RequestTransition(this, true);
  s322->addTransition(s322_req_auth);

  LoginFinishedTransition* s322_login_fault =
      new LoginFinishedTransition(this, false, s322);
  s322_login_fault->setTargetState(s321);

  sm_->addState(s11);
  sm_->addState(s12);
  sm_->addState(s2);
  sm_->addState(s3);
  sm_->setInitialState(s11);

  QSignalMapper* mapper = new QSignalMapper();
  connect(s11, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s12, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s2, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s3, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s21, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s22, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s31, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s32, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s321, SIGNAL(entered()), mapper, SLOT(map()));
  connect(s322, SIGNAL(entered()), mapper, SLOT(map()));
  mapper->setMapping(s11, "s11");
  mapper->setMapping(s12, "s12");
  mapper->setMapping(s2, "s2");
  mapper->setMapping(s3, "s3");
  mapper->setMapping(s21, "s21");
  mapper->setMapping(s22, "s22");
  mapper->setMapping(s31, "s31");
  mapper->setMapping(s32, "s32");
  mapper->setMapping(s321, "s321");
  mapper->setMapping(s322, "s322");
  connect(mapper, SIGNAL(mapped(QString)), this, SLOT(DebugSlot(QString)));

  sm_->start();
}

void GSClient::DebugSlot(QString str) {
  qLog(Debug) << "===============================" << str
              << "===========================";
}

void GSClient::Login(const QString& login, const QString& password) {
  qLog(Debug) << Q_FUNC_INFO;
  GSReply* reply =
      Request("authenticateUser", QList<Param>() << Param("username", login)
                                                 << Param("password", password),
              false);
  NewClosure(reply, SIGNAL(Finished()), this, SLOT(LoggedIn(GSReply*)), reply);
}

void GSClient::Logout() {
  user_id_.clear();

  GSReply* reply = Request("logoutUser", QList<Param>(), false);
  connect(reply, SIGNAL(Finished()), reply, SLOT(deleteLater()));
  NewClosure(reply, SIGNAL(Finished()), [this, reply]() {
    if (!reply->hasError()) {
      this->SetLoggedIn(false);
      emit this->LoginFinished(false);
    }
  });
}

void GSClient::LoggedIn(GSReply* reply) {
  qLog(Debug) << Q_FUNC_INFO;
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  QString error;

  if (result["userID"].toInt() == 0) {
    error = tr("Invalid username and/or password");
    SetLoggedIn(false);
    emit LoginFinished(false);
  } else {
    user_id_ = result["userID"].toString();
    SetLoggedIn(true);
    emit LoginFinished(true);
  }
}

void GSClient::PostEvent(QEvent* event) { sm_->postEvent(event); }

void GSClient::OnFault() { this->disconnect(SIGNAL(Ready())); }

void GSClient::DecorateRequest(QNetworkRequest& request,
                               QVariantMap& parameters) {
  QString method_name = parameters["method"].toString();
  QVariantMap header;

  SetupClient(header, method_name, kHtmlClient);

  header.insert("country", country_);
  header.insert("session", session_);
  header.insert("privacy", 0);
  header.insert("uuid", uuid_);
  parameters.insert("header", header);

  SetHeaders(request);
}

void GSClient::SetHeaders(QNetworkRequest& request) {
  request.setRawHeader("Host", "grooveshark.com");
  request.setRawHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64; rv:40.0) Gecko/20100101 Firefox/40.0");
  request.setRawHeader("Accept", "application/json, text/javascript, */*; q=0.01");
  request.setRawHeader("Accept-Language", "en-US,en;q=0.5");
  request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain; charset=UTF-8");
  request.setRawHeader("X-Requested-With", "XMLHttpRequest");
  request.setRawHeader("Referer", "http://grooveshark.com/");

  request.setRawHeader("Origin", "http://grooveshark.com/");
  request.setRawHeader("Connection", "keep-alive");
  request.setRawHeader("Pragma", "no-cache");
  request.setRawHeader("Cache-Control", "no-cache");
  // request.setRawHeader("DNT", "1");
}

void GSClient::SetupClient(QVariantMap& header, const QString& method,
                           const ClientPreset& client) {
  header.insert("client", client.client);
  header.insert("clientRevision", client.clientRevision);
  header.insert("token", CreateToken(method, client.salt));
}

QString GSClient::CreateToken(const QString& method, const QString& salt) {
  static QString prev_rnd = "";

  auto randchar = []() -> QChar {
    const QString charset = "0123456789abcdef";
    const int max_index = (charset.size() - 1);
    return charset[rand() % max_index];
  };

  QString rnd(6, 0);
  do {
    std::generate_n(rnd.begin(), 6, randchar);
  } while (rnd == prev_rnd);
  prev_rnd = rnd;

  QString plain = (QStringList() << method << ctoken_ << salt << rnd).join(":");

  QString hexhash =
      QString(QCryptographicHash::hash(plain.toUtf8(), QCryptographicHash::Sha1)
                  .toHex());
  return rnd + hexhash;
}

QNetworkReply* GSClient::makeRequest(const QString& method,
                                     const QVariantMap& parameters) {
  QNetworkRequest request(QUrl(kGSMoreUrl.arg(method)));
  QVariantMap post_params;
  post_params.insert("method", method);
  post_params.insert("parameters", parameters);
  DecorateRequest(request, post_params);

  bool ok = false;
  QJson::Serializer serializer;
  QByteArray data = serializer.serialize(post_params, &ok);
  if (!ok) {
    qLog(Error) << "Error while serializing request parameters.";
  }

  qLog(Debug) << data << "===============";
  QNetworkReply* reply = network_->post(request, data);
  return reply;
}

void GSClient::makeRequest(GSRequest* request) {
  request->getReply()->setReply(makeRequest(request->getMethod(), request->getParameters()));
}

void GSClient::ClearSession() {
  session_.clear();
  ctoken_.clear();
}

GSReply* GSClient::Request(const QString method, const QList<Param> parameters,
                           bool auth_required) {
  return Request(method, parameters, auth_required, RequestEventType);
}

GSReply* GSClient::Request(const QString method, const QList<Param> parameters,
                           bool auth_required, QEvent::Type type) {
  qLog(Debug) << Q_FUNC_INFO;
  QVariantMap params;
  for (const Param& p : parameters) {
    params.insert(p.first, p.second);
  }

  GSReply* gsreply = new GSReply(this);
  GSRequest* gsreq = new GSRequest(this, gsreply, method, params, auth_required, type);
  gsreply->setRequest(gsreq);
  if (type != SysRequestEventType) connect(this, SIGNAL(Fault()), gsreq, SLOT(CancelRequest()));

  sm_->postEvent(new ReqEvent(gsreq));

  return gsreply;
}

void GSReply::setReply(QNetworkReply* reply) {
  timer_.stop();
  reply_ = reply;
  connect(reply, SIGNAL(finished()), this, SLOT(ProcessReply()));
  connect(this, SIGNAL(destroyed()), reply_, SLOT(deleteLater()));
  connect(&timer_, SIGNAL(timeout()), this, SLOT(ProcessReply()));
  timer_.start();
}

void GSReply::setRequest(GSRequest* request) {
  request_ = request;
}

void GSReply::Cancel() {
  qLog(Debug) << Q_FUNC_INFO;
  timer_.stop();
  SetError(GSClient::Error_Cancelled, "Request cancelled.");
  emit Finished();
}

void GSReply::SetError(GSClient::Error error, const QString& msg) {
  qLog(Error) << error << msg;
  has_error_ = true;
  error_ = error;
  error_msg_ = msg;
}

bool GSReply::ProcessReplyError(const QVariantMap& result) {
  bool resend = false;
  QVariantMap fault = result["fault"].toMap();
  if (!fault.isEmpty()) {
    GSClient::Error error = GSClient::Error(fault["code"].toInt());

    switch (error) {
      case GSClient::Error_InvalidToken:
        resend = true;
        client_->ctoken_timer_->stop();
        emit client_->CTExpired();
        break;
      case GSClient::Error_InvalidSession:
      case GSClient::Error_FetchingToken:
        if (type_ == SysRequestEventType)
          has_error_ = true;
        else
          resend = true;
        client_->ClearSession();
        emit client_->SessionExpired();
        break;
      case GSClient::Error_MustBeLoggedIn:
        SetError(error, fault["message"].toString());
        break;
      default:
        break;
    }
  }
  return resend;
}

void GSReply::ProcessReply() {
  if (!timer_.isActive()) {
    SetError(GSClient::Error_HttpTimeout, "Http timeout");
    emit Finished();
    return;
  }
  if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
      200) {
    SetError(GSClient::Error_HttpError, "Http status code error");
    emit Finished();
    return;
  }

  QJson::Parser parser;
  bool ok;
  QByteArray raw = reply_->readAll();
  qLog(Debug) << raw
              << "-----------------------------------------GSReply-------"
              << request_->getMethod() << request_->getParameters() << "\n";
  QVariantMap result = parser.parse(raw, &ok).toMap();
  if (!ok) {
    SetError(GSClient::Error_ParseError,
             "Error while parsing Grooveshark result");
    emit Finished();
    return;
  }

  if (ProcessReplyError(result)) {
    client_->PostEvent(new ReqEvent(request_));
    return;
  }

  result_ = result["result"];
  emit Finished();
}


ReqEvent::ReqEvent(GSRequest *request) : QEvent(request->getType()), request_(request) {}
