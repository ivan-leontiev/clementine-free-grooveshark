#include "groovesharkclient.h"
#include "core/closure.h"
#include "core/logging.h"

#include <QNetworkAccessManager>
#include <QSettings>
#include <QNetworkReply>
#include <QTimer>
#include <QCryptographicHash>
#include <QUuid>
#include <qjson/parser.h>
#include <qjson/serializer.h>

const QString kSettingsGroup = "Grooveshark";
static const QString kGSMoreUrl = "https://grooveshark.com/more.php?%1";
static const QString kGSCoverUrl =
    "http://beta.grooveshark.com/static/amazonart/l";
static const QString kGSHomeUrl = "http://grooveshark.com/";

static const ClientPreset kMobileClient = {"mobileshark", 20120830,
                                           "gooeyFlubber"};

static const ClientPreset kJSClient = {"jsqueue", 20130520, "nuggetsOfBaller"};

GSClient::GSClient(QObject* parent)
    : QObject(parent),
      network_(new QNetworkAccessManager(this)),
      ctoken_timer_(new QTimer) {
  QSettings s;
  s.beginGroup(kSettingsGroup);

  session_ = s.value("sessionid").toString();
  user_id_ = s.value("userID").toString();
  uuid_ = QUuid::createUuid().toString().mid(1, 36).toUpper();

  ctoken_timer_->setSingleShot(true);
  ctoken_timer_->setInterval(600 * 1000);
  connect(ctoken_timer_, SIGNAL(timeout()), this, SLOT(CTokenExpired()));
}

void GSClient::CreateSession() {
  if (IsInState(State_Connecting)) return;
  setStateFlags(GSClient::State_Connecting);

  //  session_.clear();

  if (!session_.isEmpty()) {
    UpdateCommunicationToken();
    return;
  }

  qLog(Debug) << Q_FUNC_INFO;
  GSReply* gsreply = Request("initiateSession", QVariantMap());
  if (gsreply) {
    NewClosure(gsreply, SIGNAL(Finished()), this,
               SLOT(SessionCreated(GSReply*)), gsreply);
  }
  //  reply->deleteLater();
}

void GSClient::SessionCreated(GSReply* reply) {
  reply->deleteLater();

  if (reply->hasError()) {
    qLog(Error) << "Failed to create Grooveshark session: ";
    //    emit StreamError("Failed to create Grooveshark session: " +
    //    reply->error());
    return;
  }

  QString result = reply->getResult().toString();
  session_ = result;
  qLog(Debug) << "session id: " << session_;

  UpdateCommunicationToken();
}

void GSClient::UpdateCommunicationToken() {
  if (IsInState(State_UpdatingToken)) return;
  qLog(Debug) << Q_FUNC_INFO;

  setStateFlags(GSClient::State_UpdatingToken);
  QVariantMap m;
  m.insert("secretKey",
           QString(QCryptographicHash::hash(session_.toUtf8(),
                                            QCryptographicHash::Md5).toHex()));

  GSReply* gsreply = Request("getCommunicationToken", m);
  NewClosure(gsreply, SIGNAL(Finished()), this,
             SLOT(CommunicationTokenUpdated(GSReply*)), gsreply);
}

void GSClient::CommunicationTokenUpdated(GSReply* reply) {
  reply->deleteLater();

  QString result = reply->getResult().toString();
  ctoken_ = result;
  ctoken_timer_->start();
  setStateFlags(GSClient::State_Connected);
  unsetStateFlags(GSClient::State_TokenExpired | GSClient::State_UpdatingToken |
                  GSClient::State_Connecting);
  emit Connected();

  if (!user_id_.isEmpty()) {
    AuthenticateAsAuthorizedUser();
  }
}

void GSClient::AuthenticateAsAuthorizedUser() {
  if (IsInState(GSClient::State_Authenticating)) return;
  setStateFlags(GSClient::State_Authenticating);

  GSReply* reply = Request("authenticateAsAuthorizedUser",
                           QList<Param>() << Param("userID", user_id_));
  NewClosure(reply, SIGNAL(Finished()), this, SLOT(LoggedIn(GSReply*)), reply);
}

void GSClient::Login(const QString& login, const QString& password) {
  if (IsInState(GSClient::State_Authenticating)) return;
  setStateFlags(GSClient::State_Authenticating);
  GSReply* reply = Request("authenticateUser",
                           QList<Param>() << Param("username", login)
                                          << Param("password", password));
  NewClosure(reply, SIGNAL(Finished()), this, SLOT(LoggedIn(GSReply*)), reply);
}

void GSClient::Logout() {
  if (IsNotInState(GSClient::State_Authenticated)) return;
  unsetStateFlags(GSClient::State_Authenticated);
  user_id_.clear();

  GSReply* reply = Request("logoutUser", QList<Param>());
  NewClosure(reply, SIGNAL(Finished()), reply, SLOT(deleteLater()));
}

void GSClient::LoggedIn(GSReply* reply) {
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  QString error;

  if (result["userID"].toInt() == 0) {
    error = tr("Invalid username and/or password");
    unsetStateFlags(GSClient::State_Authenticating |
                    GSClient::State_Authenticated);
    emit LoginFinished(false);
  } else {
    user_id_ = result["userID"].toString();
    unsetStateFlags(GSClient::State_Authenticating);
    setStateFlags(GSClient::State_Authenticated);
    emit LoginFinished(true);
  }
}

void GSClient::DecorateRequest(QNetworkRequest& request,
                               QVariantMap& parameters) {
  QString method_name = parameters["method"].toString();
  QVariantMap header;

  if (method_name == "getStreamKeyFromSongIDEx")
    SetupClient(header, method_name, kMobileClient);
  else
    SetupClient(header, method_name, kJSClient);
  //    SetupClient(header, method_name, kJSClient);

  header.insert("country", QVariantMap());
  header.insert("session", session_);
  header.insert("privacy", 0);
  header.insert("uuid", uuid_);
  parameters.insert("header", header);

  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("DNT", "1");
  request.setRawHeader("X-Requested-With", "XMLHttpRequest");
  request.setRawHeader("Host", "grooveshark.com");
  request.setRawHeader("Referer", "http://grooveshark.com");
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

  QString plain = method + ":" + ctoken_ + ":" + salt + ":" + rnd;
  QString hexhash =
      QString(QCryptographicHash::hash(plain.toUtf8(), QCryptographicHash::Sha1)
                  .toHex());
  return rnd + hexhash;
}

void GSClient::setStateFlags(int state) {
  state_ = GSClient::States(state_ | state);
}

void GSClient::unsetStateFlags(int state) { state_ &= ~state; }

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

void GSClient::ClearSession() {
  session_.clear();
  ctoken_.clear();
}

// void GSClient::Connect() {
//  CreateSession();
//}

GSReply* GSClient::Request(const QString& method, const QVariantMap& parameters,
                           bool auth_required) {
  qLog(Debug) << Q_FUNC_INFO;

  GSReply* gsreply = new GSReply(this);
  gsreply->setRequest(method, parameters);

  if ((IsNotInState(State_Connected) || IsInState(State_Connecting) ||
       IsInState(State_TokenExpired) || IsInState(State_UpdatingToken)) &&
      !(method == "initiateSession" || method == "getCommunicationToken")) {
    qLog(Debug) << "deffered request" << state_ << method;
    NewClosure(this, SIGNAL(Connected()), this,
               SLOT(DefferedRequest(QString, QVariantMap, GSReply*)), method,
               parameters, gsreply);
    CreateSession();
    return gsreply;
  }

  if (auth_required) {
    if (IsNotInState(GSClient::State_Authenticated |
                     GSClient::State_Authenticating)) {
      delete gsreply;
      return nullptr;
    }

    if (IsInState(GSClient::State_Authenticating)) {
      NewClosure(this, SIGNAL(LoginFinished(bool)), this,
                 SLOT(DefferedRequest(QString, QVariantMap, GSReply*)), method,
                 parameters, gsreply);
      return gsreply;
    }
  }

  qLog(Debug) << "usual request" << state_;

  gsreply->setReply(makeRequest(method, parameters));

  //  NewClosure(reply, SIGNAL(finished()), this, SLOT(ProcessReply(GSReply*,
  //  QNetworkReply*)), gsreply, reply);

  return gsreply;
}

GSReply* GSClient::Request(const QString& method,
                           const QList<Param>& parameters, bool auth_required) {
  QVariantMap params;
  for (const Param& p : parameters) {
    params.insert(p.first, p.second);
  }
  return Request(method, params, auth_required);
}

void GSReply::setReply(QNetworkReply* reply) {
  reply_ = reply;
  connect(reply, SIGNAL(finished()), this, SLOT(ProcessReply()));
}

void GSReply::setRequest(const QString& method, const QVariantMap& parameters) {
  method_ = method;
  parameters_ = parameters;
}

bool GSReply::ProcessError(const QVariantMap& result) {
  bool resend = false;
  QVariantMap fault = result["fault"].toMap();
  if (!fault.isEmpty()) {
    has_error_ = true;
    error_ = GSClient::Error(fault["code"].toInt());
    error_msg_ = fault["message"].toString();

    qLog(Debug) << fault["code"].toString() << fault["message"].toString()
                << "!!!!!!!!!!!!!";

    switch (error_) {
      case GSClient::Error_InvalidToken:
        resend = true;
        client_->setStateFlags(GSClient::State_TokenExpired);
        //      client_->DefferedRequest(method_, parameters_, this);
        client_->UpdateCommunicationToken();
        NewClosure(client_, SIGNAL(Connected()), client_,
                   SLOT(DefferedRequest(QString, QVariantMap, GSReply*)),
                   method_, parameters_, this);
        break;
      case GSClient::Error_InvalidSession:
      case GSClient::Error_FetchingToken:
        resend = true;
        client_->unsetStateFlags(GSClient::State_Connected);
        client_->ClearSession();
        client_->CreateSession();
        //      client_->DefferedRequest(method_, parameters_, this);
        NewClosure(client_, SIGNAL(Connected()), client_,
                   SLOT(DefferedRequest(QString, QVariantMap, GSReply*)),
                   method_, parameters_, this);
        break;
      case GSClient::Error_MustBeLoggedIn:
        client_->user_id_.clear();
        client_->unsetStateFlags(GSClient::State_Authenticated);
        break;
      default:
        break;
    }
  }
  return resend;
}

// GSReply::~GSReply() {
//  reply_->deleteLater();
//}

void GSReply::ProcessReply() {
  reply_->deleteLater();

  if (reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
      200) {
    has_error_ = true;
    error_ = GSClient::Error_HttpError;
    error_msg_ = "";
    emit Finished();
    return;
  }

  QJson::Parser parser;
  bool ok;
  QByteArray raw = reply_->readAll();
  qLog(Debug) << raw
              << "-----------------------------------------GSReply-------"
              << method_ << parameters_ << "\n";
  //  QVariantMap result = parser.parse(reply, &ok).toMap();
  QVariantMap result = parser.parse(raw, &ok).toMap();
  if (!ok) {
    qLog(Error) << "Error while parsing Grooveshark result";
  }

  if (ProcessError(result)) return;

  result_ = result["result"];
  emit Finished();
}

void GSClient::CTokenExpired() {
  qLog(Debug) << Q_FUNC_INFO;
  setStateFlags(GSClient::State_TokenExpired);
}

void GSClient::DefferedRequest(const QString& method,
                               const QVariantMap& parameters,
                               GSReply* deffered) {
  qLog(Debug) << Q_FUNC_INFO;
  deffered->setReply(makeRequest(method, parameters));
}
