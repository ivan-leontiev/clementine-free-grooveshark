#ifndef INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H
#define INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H

#include "core/logging.h"
#include "core/closure.h"

#include <QObject>
#include <QVariantMap>
#include <QNetworkRequest>
#include <QStateMachine>
#include <QTimer>
#include <QSignalTransition>
#include <QEvent>

class QNetworkAccessManager;
class QNetworkReply;
// class QTimer;
class GSReply;

typedef QPair<QString, QVariant> Param;

struct ClientPreset {
  QString client;
  int clientRevision;
  QString salt;
};

class GSClient : public QObject {
  Q_OBJECT
  Q_FLAGS(States)
  Q_ENUMS(Error)
  Q_PROPERTY(bool logged_in READ IsLoggedIn WRITE SetLoggedIn)

  friend class GSReply;

 public:
  explicit GSClient(QObject* parent = 0);

  enum Error {
    Error_FetchingToken = 0,
    Error_InvalidType = 1,
    Error_HttpError = 2,
    Error_ParseError = 4,
    Error_HttpTimeout = 6,
    Error_MustBeLoggedIn = 8,
    Error_Maintenance = 10,
    Error_InvalidSession = 16,
    Error_InvalidToken = 256,
    Error_RateLimited = 512,
    Error_InvalidClient = 1024,
    Error_Cancelled = 333
  };

  void Login(const QString& login, const QString& password);
  void Logout();
  bool IsLoggedIn() { return logged_in_; }
  GSReply* Request(const QString method, const QList<Param> parameters,
                   bool auth_required = false);
  const QString& getSessionID() { return session_; }
  const QString& getUserID() { return user_id_; }

 public slots:
  void SetLoggedIn(bool b) { logged_in_ = b; }
  void makeRequest(GSReply* reply);
  void DebugSlot(QString);

  void DebugSlotMark();
signals:
  void Ready();
  void LoginFinished(bool success);
  void Ok();
  void CTExpired();
  void Fault();

 private:
  static const int kCTokenTimeout;

  QNetworkAccessManager* network_;
  QStateMachine* sm_;

  QString session_;
  QString ctoken_;
  QTimer* ctoken_timer_;
  QString user_id_;
  QString uuid_;
  QVariantMap country_;
  bool logged_in_;

 private:
  GSReply* Request(const QString method, const QList<Param> parameters,
                   bool auth_required, QEvent::Type type);
  void DecorateRequest(QNetworkRequest& request, QVariantMap& parameters);
  void SetupClient(QVariantMap& header, const QString& method,
                   const ClientPreset& client);
  QString CreateToken(const QString& method, const QString& salt);
  void WaitForReply(QNetworkReply* reply);
  void ClearSession();
  void SetupSM();

 private slots:
  void CreateSession();
  void UpdateCommunicationToken();
  void AuthenticateAsAuthorizedUser();
  QNetworkReply* makeRequest(const QString& method,
                             const QVariantMap& parameters);
  void SessionCreated(GSReply* reply);
  void CommunicationTokenUpdated(GSReply* reply);
  void CTokenExpired();
  void LoggedIn(GSReply* reply);
  void PostEvent(QEvent* event);
  void OnFault();
};

class GSReply : public QObject {
  Q_OBJECT

  friend class RequestTransition;

 public:
  QString method_;
  QVariantMap parameters_;
  bool auth_required_;

 public:
  explicit GSReply(GSClient* client)
      : has_error_(false), resending_(false), client_(client) {
    timer_.setInterval(kGSReplyTimeout);
  }

  bool hasError() { return has_error_; }
  QVariant getResult() const { return result_; }
  void setReply(QNetworkReply* reply);
  void setRequest(const QString method, const QVariantMap parameters,
                  bool auth_required, QEvent::Type type);
  void Cancel();

signals:
  void Finished();

 private:
  bool ProcessReplyError(const QVariantMap& result);
  void SetError(GSClient::Error error, const QString& msg);

 private slots:
  void ProcessReply();

 private:
  static const int kGSReplyTimeout;

  bool has_error_;
  bool resending_;
  QEvent::Type type_;
  QVariant result_;
  QNetworkReply* reply_;
  GSClient* client_;
  GSClient::Error error_;
  QString error_msg_;
  QTimer timer_;
};

// Q_DECLARE_METATYPE(GSReply*)

const QEvent::Type RequestEventType = QEvent::Type(QEvent::User + 1);
const QEvent::Type SysRequestEventType = QEvent::Type(QEvent::User + 2);

class LoginFinishedTransition : public QSignalTransition {
 public:
  LoginFinishedTransition(GSClient* sender, bool ok, QState* sourceState = 0)
      : QSignalTransition(sender, SIGNAL(LoginFinished(bool)), sourceState),
        ok_(ok) {}

  virtual bool eventTest(QEvent* e) {
    if (!QSignalTransition::eventTest(e)) return false;
    QStateMachine::SignalEvent* se =
        static_cast<QStateMachine::SignalEvent*>(e);

    return ok_ == se->arguments().at(0).toBool();
  }

 private:
  bool ok_;
};

class ReqEvent : public QEvent {
 public:
  ReqEvent(Type type, GSReply* reply) : QEvent(type), reply_(reply) {}
  GSReply* getReply() { return reply_; }
  QString getMethod() { return reply_->method_; }
  bool isAuthRequired() { return reply_->auth_required_; }

 protected:
  GSReply* reply_;
};

class RequestTransition : public QAbstractTransition {
 public:
  RequestTransition(GSClient* client, bool auth_required)
      : client_(client), auth_required_(auth_required), check_auth_(true) {}
  RequestTransition(GSClient* client) : client_(client), check_auth_(false) {}

  virtual bool eventTest(QEvent* event) {
    GSReply* reply = static_cast<ReqEvent*>(event)->getReply();
    return (event->type() == RequestEventType ||
            event->type() == SysRequestEventType) &&
           (!check_auth_ || (reply->auth_required_ == auth_required_));
  }

  virtual void onTransition(QEvent* event) {
    GSReply* reply = static_cast<ReqEvent*>(event)->getReply();
    client_->makeRequest(reply);
  }

 protected:
  GSClient* client_;
  bool auth_required_;
  bool check_auth_;
};

class DeferRequestTransition : public RequestTransition {
 public:
  DeferRequestTransition(GSClient* client, bool auth_required)
      : RequestTransition(client, auth_required) {}
  DeferRequestTransition(GSClient* client) : RequestTransition(client) {}

  virtual void onTransition(QEvent* event) {
    ReqEvent* re = static_cast<ReqEvent*>(event);
    if (re->type() == SysRequestEventType) {
      RequestTransition::onTransition(event);
    } else {
      ReqEvent* new_re = new ReqEvent(RequestEventType, re->getReply());
      NewClosure(client_, SIGNAL(Ready()), client_, SLOT(PostEvent(QEvent*)),
                 new_re);
      NewClosure(client_, SIGNAL(Fault()), [new_re]() {
        new_re->getReply()->Cancel();
        delete new_re;
      });
    }
  }
};

class CancelRequestTransition : public RequestTransition {
 public:
  CancelRequestTransition(GSClient* client, bool auth_required_)
      : RequestTransition(client, auth_required_) {}

  virtual void onTransition(QEvent* event) {
    GSReply* reply = static_cast<ReqEvent*>(event)->getReply();
    reply->Cancel();
  }
};

#endif  // INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H
