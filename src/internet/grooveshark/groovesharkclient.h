#ifndef INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H
#define INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H

#include <QObject>
#include <QVariantMap>
#include <QNetworkRequest>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class GSReply;

typedef QPair<QString, QVariant> Param;

struct ClientPreset {
  QString client;
  int clientRevision;
  QString salt;
};


class GSClient : public QObject
{
  Q_OBJECT
  Q_FLAGS(States)
  Q_ENUMS(Error)

  friend class GSReply;

 public:
  explicit GSClient(QObject *parent = 0);

  enum State {
    State_LoggedIn          = 1 << 0,
    State_UpdatingToken     = 1 << 1,
    State_TokenExpired      = 1 << 2,
    State_Connecting        = 1 << 3,
    State_Connected         = 1 << 4,
    State_Authenticating    = 1 << 5,
    State_Authenticated     = 1 << 6
  };
  Q_DECLARE_FLAGS(States, State)

  enum Error {
    Error_FetchingToken     = 0,
    Error_InvalidType       = 1,
    Error_HttpError         = 2,
    Error_ParseError        = 4,
    Error_HttpTimeout       = 6,
    Error_MustBeLoggedIn    = 8,
    Error_Maintenance       = 10,
    Error_InvalidSession    = 16,
    Error_InvalidToken      = 256,
    Error_RateLimited       = 512,
    Error_InvalidClient     = 1024
  };

//  void Connect();
  void Login(const QString &login, const QString &password);
  void Logout();
//  bool IsConnected() { return IsInState(GSClient::State_Connected); }
  bool IsLoggedIn() { return IsInState(GSClient::State_Authenticated); }
  bool IsInState(int states) { return (state_ & states) == states; }
  bool IsNotInState(int states) { return (state_ & ~states) == state_; }
  GSReply* Request(const QString& method, const QVariantMap &parameters, bool auth_required=false);
  GSReply* Request(const QString& method, const QList<Param> &parameters, bool auth_required=false);
  const QString& getSessionID() { return session_; }
  const QString& getUserID() { return user_id_; }

 signals:
  void Connected();
  void LoginFinished(bool success);

 private:
  QNetworkAccessManager* network_;
  GSClient::States state_;

//  QString username_;
//  QString password_;

  QString session_;
  QString ctoken_;
//  QDateTime ctoken_expiry_time_;
  QTimer* ctoken_timer_;
  QString user_id_;
  QString uuid_;
  QVariantMap country_;

 private:
  void CreateSession();
  void UpdateCommunicationToken();
  void DecorateRequest(QNetworkRequest& request, QVariantMap& parameters);
  void SetupClient(QVariantMap& header, const QString &method, const ClientPreset& client);
  QString CreateToken(const QString& method, const QString& salt);
  void setStateFlags(int state);
  void unsetStateFlags(int state);
  void WaitForReply(QNetworkReply* reply);
  QNetworkReply *makeRequest(const QString& method, const QVariantMap& parameters);
  void ClearSession();
  void AuthenticateAsAuthorizedUser();

 private slots:
  void SessionCreated(GSReply *reply);
  void CommunicationTokenUpdated(GSReply *reply);
  void CTokenExpired();
  void DefferedRequest(const QString& method, const QVariantMap &parameters, GSReply* deffered);
  void LoggedIn(GSReply* reply);
};

class GSReply : public QObject
{
  Q_OBJECT

 public:
  explicit GSReply(GSClient *client) : has_error_(false), resending_(false), client_(client) {}

  bool hasError() { return has_error_; }
  QVariant getResult() const { return result_; }
  void setReply(QNetworkReply* reply);
  void setRequest(const QString& method, const QVariantMap& parameters);
//  GSClient::Error getError() const { return error_; }

 signals:
  void Finished();

 private:
  bool ProcessError(const QVariantMap &result);

 private slots:
  void ProcessReply();

 private:
  bool has_error_;
  bool resending_;
  QVariant result_;
  QNetworkReply* reply_;
  GSClient* client_;
  QString method_;
  QVariantMap parameters_;
  GSClient::Error error_;
  QString error_msg_;
};

#endif // INTERNET_GROOVESHARK_GROOVESHARKCLIENT_H
