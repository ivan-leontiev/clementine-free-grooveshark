/* This file is part of Clementine.
   Copyright 2011-2014, Arnaud Bienner <arnaud.bienner@gmail.com>
   Copyright 2011-2012, 2014, John Maguire <john.maguire@gmail.com>
   Copyright 2011, HYPNOTOAD <hypnotoad@clementine.org>
   Copyright 2011-2012, David Sansome <me@davidsansome.com>
   Copyright 2014, Antonio Nicolás Pina <antonio@antonionicolaspina.com>
   Copyright 2014, Krzysztof Sobiecki <sobkas@gmail.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "groovesharkservice.h"

#include <memory>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTimer>

#include <qjson/parser.h>
#include <qjson/serializer.h>

#include "qtiocompressor.h"

#include "internet/core/internetmodel.h"
#include "groovesharkradio.h"
#include "groovesharkurlhandler.h"
#include "internet/core/searchboxwidget.h"

#include "core/application.h"
#include "core/closure.h"
#include "core/database.h"
#include "core/logging.h"
#include "core/mergedproxymodel.h"
#include "core/network.h"
#include "core/player.h"
#include "core/scopedtransaction.h"
#include "core/song.h"
#include "core/taskmanager.h"
#include "core/timeconstants.h"
#include "core/utilities.h"
#include "globalsearch/globalsearch.h"
#include "globalsearch/groovesharksearchprovider.h"
#include "playlist/playlist.h"
#include "playlist/playlistcontainer.h"
#include "playlist/playlistmanager.h"
#include "ui/iconloader.h"

using smart_playlists::Generator;
using smart_playlists::GeneratorPtr;

const char* GroovesharkService::kServiceName = "Grooveshark";
const char* GroovesharkService::kSettingsGroup = "Grooveshark";
const char* GroovesharkService::kUrlCover =
    "http://beta.grooveshark.com/static/amazonart/l";
const char* GroovesharkService::kHomepage = "http://grooveshark.com/";

const int GroovesharkService::kSongSimpleSearchLimit = 10;
const int GroovesharkService::kSearchDelayMsec = 1000;

typedef QPair<QString, QVariant> Param;

GroovesharkService::GroovesharkService(Application* app, InternetModel* parent)
    : InternetService(kServiceName, app, parent, parent),
      url_handler_(new GroovesharkUrlHandler(this, this)),
      next_pending_search_id_(0),
      next_pending_playlist_retrieve_id_(0),
      root_(nullptr),
      search_(nullptr),
      popular_month_(nullptr),
      popular_today_(nullptr),
      stations_(nullptr),
      grooveshark_radio_(nullptr),
      favorites_(nullptr),
      library_(nullptr),
      playlists_parent_(nullptr),
      subscribed_playlists_parent_(nullptr),
      client_(new GSClient(this)),
      context_menu_(nullptr),
      create_playlist_(nullptr),
      delete_playlist_(nullptr),
      rename_playlist_(nullptr),
      remove_from_playlist_(nullptr),
      remove_from_favorites_(nullptr),
      remove_from_library_(nullptr),
      get_url_to_share_song_(nullptr),
      get_url_to_share_playlist_(nullptr),
      search_box_(new SearchBoxWidget(this)),
      search_delay_(new QTimer(this)),
      last_search_reply_(nullptr),
      login_state_(LoginState_OtherError),
      task_popular_id_(0),
      task_playlists_id_(0),
      task_search_id_(0) {
  app_->player()->RegisterUrlHandler(url_handler_);

  search_delay_->setInterval(kSearchDelayMsec);
  search_delay_->setSingleShot(true);

  connect(search_delay_, SIGNAL(timeout()), SLOT(DoSearch()));

  GroovesharkSearchProvider* search_provider =
      new GroovesharkSearchProvider(app_, this);
  search_provider->Init(this);
  app_->global_search()->AddProvider(search_provider);

  connect(search_box_, SIGNAL(TextChanged(QString)), SLOT(Search(QString)));

  connect(client_, SIGNAL(LoginFinished(bool)), this,
          SIGNAL(LoginFinished(bool)));
}

GroovesharkService::~GroovesharkService() {}

QStandardItem* GroovesharkService::CreateRootItem() {
  root_ = new QStandardItem(QIcon(":providers/grooveshark.png"), kServiceName);
  root_->setData(true, InternetModel::Role_CanLazyLoad);
  root_->setData(InternetModel::PlayBehaviour_DoubleClickAction,
                 InternetModel::Role_PlayBehaviour);
  return root_;
}

void GroovesharkService::LazyPopulate(QStandardItem* item) {
  switch (item->data(InternetModel::Role_Type).toInt()) {
    case InternetModel::Type_Service: {
      EnsureConnected();
      break;
    }
    default:
      break;
  }
}

void GroovesharkService::ShowConfig() {
  app_->OpenSettingsDialogAtPage(SettingsDialog::Page_Grooveshark);
}

QWidget* GroovesharkService::HeaderWidget() const { return search_box_; }

void GroovesharkService::Search(const QString& text, bool now) {
  pending_search_ = text;

  // If there is no text (e.g. user cleared search box), we don't need to do a
  // real query that will return nothing: we can clear the playlist now
  if (text.isEmpty()) {
    search_delay_->stop();
    ClearSearchResults();
    return;
  }

  if (now) {
    search_delay_->stop();
    DoSearch();
  } else {
    search_delay_->start();
  }
}

int GroovesharkService::SimpleSearch(const QString& query) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  //  QList<Param> headers;
  parameters << Param("query", query)
             << Param("type", QVariantList() << "Songs"
                                             << "Albums") << Param("guts", 0)
             << Param("ppOverride", false);
  //             << Param("limit", QString::number(1));
  //             << Param("offset", "");

  //  headers << Param("client", "mobileshark") << Param("clientRevision",
  //  20120830);
  int id = next_pending_search_id_++;
  //  QNetworkReply* reply = CreateRequestOld("getSongSearchResults",
  //  parameters);
  GSReply* reply = client_->Request("getResultsFromSearch", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SimpleSearchFinished(GSReply*, int)), reply, id);
  return id;
}

void GroovesharkService::SimpleSearchFinished(GSReply* reply, int id) {
  qLog(Debug) << Q_FUNC_INFO;
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap()["result"].toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);
  emit SimpleSearchResults(id, songs);
}

int GroovesharkService::SearchAlbums(const QString& query) {
  QList<Param> parameters;
  parameters << Param("query", query)
             << Param("type", QVariantList() << "Albums");

  //  QNetworkReply* reply = CreateRequestOld("getAlbumSearchResults",
  //  parameters);
  GSReply* reply = client_->Request("getResultsFromSearch", parameters);

  const int id = next_pending_search_id_++;

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SearchAlbumsFinished(GSReply*, int)), reply, id);

  return id;
}

void GroovesharkService::SearchAlbumsFinished(GSReply* reply, int id) {
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  QVariantList albums = result["result"].toMap()["Albums"].toList();

  QList<quint64> ret;
  for (const QVariant& v : albums) {
    quint64 album_id = v.toMap()["AlbumID"].toULongLong();
    GetAlbumSongs(album_id);
    ret << album_id;
  }

  emit AlbumSearchResult(id, ret);
}

void GroovesharkService::GetAlbumSongs(quint64 album_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("albumID", album_id);
  //  QNetworkReply* reply = CreateRequestOld("getAlbumSongs", parameters);
  GSReply* reply = client_->Request("albumGetAllSongs", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(GetAlbumSongsFinished(GSReply*, quint64)), reply, album_id);
}

void GroovesharkService::GetAlbumSongsFinished(GSReply* reply,
                                               quint64 album_id) {
  reply->deleteLater();
  QVariantList result = reply->getResult().toList();
  SongList songs = ExtractSongs(QVariantMap(), result, true);

  emit AlbumSongsLoaded(album_id, songs);
}

void GroovesharkService::DoSearch() {
  qLog(Debug) << Q_FUNC_INFO;
  if (!task_search_id_) {
    task_search_id_ =
        app_->task_manager()->StartTask(tr("Searching on Grooveshark"));
  }

  ClearSearchResults();

  QList<Param> parameters;
  //  QList<Param> headers;
  parameters << Param("query", pending_search_)
             << Param("type", QVariantList() << "Songs") << Param("guts", 0)
             << Param("ppOverride", false);
  //  headers << Param("client", "mobileshark") << Param("clientRevision",
  //  20120830);
  //             << Param("limit", QString::number(kSongSearchLimit))
  //             << Param("offset", "");
  //  last_search_reply_ = CreateRequestOld("getSongSearchResults", parameters);
  last_search_reply_ = client_->Request("getResultsFromSearch", parameters);
  NewClosure(last_search_reply_, SIGNAL(Finished()), this,
             SLOT(SearchSongsFinished(GSReply*)), last_search_reply_);
}

void GroovesharkService::SearchSongsFinished(GSReply* reply) {
  reply->deleteLater();

  if (reply != last_search_reply_) return;

  QVariantMap result = reply->getResult().toMap()["result"].toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);
  app_->task_manager()->SetTaskFinished(task_search_id_);
  task_search_id_ = 0;

  // Fill results list
  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    search_->appendRow(child);
  }

  QModelIndex index = model()->merged_model()->mapFromSource(search_->index());
  ScrollToIndex(index);
}

QUrl GroovesharkService::GetStreamingUrlFromSongId(const QString& song_id,
                                                   const QString& artist_id,
                                                   QString* server_id,
                                                   QString* stream_key,
                                                   qint64* length_nanosec) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;

  parameters << Param("songID", song_id) << Param("country", QVariantMap())
             << Param("prefetch", false) << Param("type", 0)
             << Param("mobile", false);
  GSReply* reply = client_->Request("getStreamKeyFromSongIDEx", parameters);

  // Wait for the reply
  bool reply_has_timeouted = !WaitForGSReply(reply);
  reply->deleteLater();
  if (reply_has_timeouted) return QUrl();

  QVariantMap result = reply->getResult().toMap();
  server_id->clear();
  server_id->append(result["streamServerID"].toString());
  stream_key->clear();
  stream_key->append(result["streamKey"].toString());
  *length_nanosec = result["uSecs"].toLongLong() * 1000;
  // Keep in mind that user has request to listen to this song
  last_songs_ids_.append(song_id.toInt());
  last_artists_ids_.append(artist_id.toInt());
  // If we have enough ids, remove the old ones
  if (last_songs_ids_.size() > 100) last_songs_ids_.removeFirst();
  if (last_artists_ids_.size() > 100) last_artists_ids_.removeFirst();

  QUrl url;
  url.setScheme("http");
  url.setHost(result["ip"].toString());
  url.setPath("/stream.php");
  url.setQueryItems(QList<QPair<QString, QString>>() << QPair<QString, QString>(
                        "streamKey", result["streamKey"].toString()));
  //  return QUrl("http://" + result["ip"].toString() + "/" +
  //  "stream.php?streamKey=" + result["streamKey"].toString());
  return url;
}

void GroovesharkService::Login(const QString& username,
                               const QString& password) {
  qLog(Debug) << Q_FUNC_INFO;
  client_->Login(username, password);
  NewClosure(client_, SIGNAL(LoginFinished(bool)), this, SLOT(RefreshItems()));
}

void GroovesharkService::ClearSearchResults() {
  if (search_) search_->removeRows(0, search_->rowCount());
}

void GroovesharkService::Logout() {
  qLog(Debug) << Q_FUNC_INFO;
  client_->Logout();
  RemoveItems();
  RefreshItems();
}

void GroovesharkService::RemoveItems() {
  root_->removeRows(0, root_->rowCount());
  // 'search', 'favorites', 'popular', ... items were root's children, and have
  // been deleted: we should update these now invalid pointers
  search_ = nullptr;
  popular_month_ = nullptr;
  popular_today_ = nullptr;
  library_ = nullptr;
  favorites_ = nullptr;
  subscribed_playlists_parent_ = nullptr;
  stations_ = nullptr;
  grooveshark_radio_ = nullptr;
  playlists_parent_ = nullptr;
  playlists_.clear();
  subscribed_playlists_parent_ = nullptr;
  subscribed_playlists_.clear();
  // Cancel any pending requests and mark tasks as finished, in case they
  // weren't
  // finished yet.
  pending_retrieve_playlists_.clear();
  app_->task_manager()->SetTaskFinished(task_playlists_id_);
  app_->task_manager()->SetTaskFinished(task_popular_id_);
  app_->task_manager()->SetTaskFinished(task_search_id_);
}

void GroovesharkService::ShowContextMenu(const QPoint& global_pos) {
  EnsureMenuCreated();

  // Check if we should display actions
  bool display_delete_playlist_action = false,
       display_remove_from_playlist_action = false,
       display_remove_from_favorites_action = false,
       display_remove_from_library_action = false,
       display_share_song_url = false, display_share_playlist_url = false;

  QModelIndex index(model()->current_index());

  if (index.data(InternetModel::Role_Type).toInt() ==
          InternetModel::Type_UserPlaylist &&
      index.data(Role_PlaylistType).toInt() == UserPlaylist) {
    display_delete_playlist_action = true;
  }
  // We check parent's type (instead of index type) because we want to enable
  // 'remove' actions for items which are inside a playlist
  int parent_type = index.parent().data(InternetModel::Role_Type).toInt();
  if (parent_type == InternetModel::Type_UserPlaylist) {
    int parent_playlist_type = index.parent().data(Role_PlaylistType).toInt();
    if (parent_playlist_type == UserFavorites)
      display_remove_from_favorites_action = true;
    else if (parent_playlist_type == UserLibrary)
      display_remove_from_library_action = true;
    else if (parent_playlist_type == UserPlaylist)
      display_remove_from_playlist_action = true;
  }
  delete_playlist_->setVisible(display_delete_playlist_action);
  // If we can delete this playlist, we can also rename it
  rename_playlist_->setVisible(display_delete_playlist_action);
  remove_from_playlist_->setVisible(display_remove_from_playlist_action);
  remove_from_favorites_->setVisible(display_remove_from_favorites_action);
  remove_from_library_->setVisible(display_remove_from_library_action);

  // Check if we can display actions to get URL for sharing songs/playlists:
  // - share song
  if (index.data(InternetModel::Role_Type).toInt() ==
      InternetModel::Type_Track) {
    display_share_song_url = true;
    current_song_info_ =
        ExtractSongInfoFromUrl(index.data(InternetModel::Role_Url).toUrl());
  }
  get_url_to_share_song_->setVisible(display_share_song_url);

  // - share playlist
  if (index.data(InternetModel::Role_Type).toInt() ==
          InternetModel::Type_UserPlaylist &&
      index.data(Role_UserPlaylistId).isValid()) {
    display_share_playlist_url = true;
    current_playlist_id_ = index.data(Role_UserPlaylistId).toInt();
  } else if (parent_type == InternetModel::Type_UserPlaylist &&
             index.parent().data(Role_UserPlaylistId).isValid()) {
    display_share_playlist_url = true;
    current_playlist_id_ = index.parent().data(Role_UserPlaylistId).toInt();
  }
  get_url_to_share_playlist_->setVisible(display_share_playlist_url);

  context_menu_->popup(global_pos);
}

void GroovesharkService::EnsureMenuCreated() {
  if (!context_menu_) {
    context_menu_ = new QMenu;
    context_menu_->addActions(GetPlaylistActions());
    create_playlist_ = context_menu_->addAction(
        IconLoader::Load("list-add"), tr("Create a new Grooveshark playlist"),
        this, SLOT(CreateNewPlaylist()));
    delete_playlist_ = context_menu_->addAction(
        IconLoader::Load("edit-delete"), tr("Delete Grooveshark playlist"),
        this, SLOT(DeleteCurrentPlaylist()));
    rename_playlist_ = context_menu_->addAction(
        IconLoader::Load("edit-rename"), tr("Rename Grooveshark playlist"),
        this, SLOT(RenameCurrentPlaylist()));
    context_menu_->addSeparator();
    remove_from_playlist_ = context_menu_->addAction(
        IconLoader::Load("list-remove"), tr("Remove from playlist"), this,
        SLOT(RemoveCurrentFromPlaylist()));
    remove_from_favorites_ = context_menu_->addAction(
        IconLoader::Load("list-remove"), tr("Remove from favorites"), this,
        SLOT(RemoveCurrentFromFavorites()));
    remove_from_library_ = context_menu_->addAction(
        IconLoader::Load("list-remove"), tr("Remove from My Music"), this,
        SLOT(RemoveCurrentFromLibrary()));
    get_url_to_share_song_ =
        context_menu_->addAction(tr("Get a URL to share this Grooveshark song"),
                                 this, SLOT(GetCurrentSongUrlToShare()));
    get_url_to_share_playlist_ = context_menu_->addAction(
        tr("Get a URL to share this Grooveshark playlist"), this,
        SLOT(GetCurrentPlaylistUrlToShare()));
    context_menu_->addSeparator();
    context_menu_->addAction(IconLoader::Load("download"),
                             tr("Open %1 in browser").arg("grooveshark.com"),
                             this, SLOT(Homepage()));
    context_menu_->addAction(IconLoader::Load("view-refresh"), tr("Refresh"),
                             this, SLOT(RefreshItems()));
    context_menu_->addSeparator();
    context_menu_->addAction(IconLoader::Load("configure"),
                             tr("Configure Grooveshark..."), this,
                             SLOT(ShowConfig()));
  }
}

void GroovesharkService::Homepage() {
  QDesktopServices::openUrl(QUrl(kHomepage));
}

void GroovesharkService::RefreshItems() {
  RemoveItems();
  EnsureItemsCreated();
}

void GroovesharkService::EnsureItemsCreated() {
  if (!search_) {
    search_ =
        new QStandardItem(IconLoader::Load("edit-find"), tr("Search results"));
    search_->setToolTip(
        tr("Start typing something on the search box above to "
           "fill this search results list"));
    search_->setData(InternetModel::PlayBehaviour_MultipleItems,
                     InternetModel::Role_PlayBehaviour);
    root_->appendRow(search_);

    QStandardItem* popular =
        new QStandardItem(QIcon(":/star-on.png"), tr("Popular songs"));
    root_->appendRow(popular);

    popular_month_ = new QStandardItem(QIcon(":/star-on.png"),
                                       tr("Popular songs of the Month"));
    popular_month_->setData(InternetModel::Type_UserPlaylist,
                            InternetModel::Role_Type);
    popular_month_->setData(true, InternetModel::Role_CanLazyLoad);
    popular_month_->setData(InternetModel::PlayBehaviour_MultipleItems,
                            InternetModel::Role_PlayBehaviour);
    popular->appendRow(popular_month_);

    popular_today_ =
        new QStandardItem(QIcon(":/star-on.png"), tr("Popular songs today"));
    popular_today_->setData(InternetModel::Type_UserPlaylist,
                            InternetModel::Role_Type);
    popular_today_->setData(true, InternetModel::Role_CanLazyLoad);
    popular_today_->setData(InternetModel::PlayBehaviour_MultipleItems,
                            InternetModel::Role_PlayBehaviour);
    popular->appendRow(popular_today_);

    QStandardItem* radios_divider =
        new QStandardItem(QIcon(":last.fm/icon_radio.png"), tr("Radios"));
    root_->appendRow(radios_divider);

    stations_ =
        new QStandardItem(QIcon(":last.fm/icon_radio.png"), tr("Stations"));
    stations_->setData(InternetModel::Type_UserPlaylist,
                       InternetModel::Role_Type);
    stations_->setData(true, InternetModel::Role_CanLazyLoad);
    radios_divider->appendRow(stations_);

    grooveshark_radio_ = new QStandardItem(QIcon(":last.fm/icon_radio.png"),
                                           tr("Grooveshark radio"));
    grooveshark_radio_->setToolTip(
        tr("Listen to Grooveshark songs based on what you've listened to "
           "previously"));
    grooveshark_radio_->setData(InternetModel::Type_SmartPlaylist,
                                InternetModel::Role_Type);
    radios_divider->appendRow(grooveshark_radio_);

    library_ =
        new QStandardItem(IconLoader::Load("folder-sound"), tr("My Music"));
    library_->setData(InternetModel::Type_UserPlaylist,
                      InternetModel::Role_Type);
    library_->setData(UserLibrary, Role_PlaylistType);
    library_->setData(true, InternetModel::Role_CanLazyLoad);
    library_->setData(true, InternetModel::Role_CanBeModified);
    library_->setData(InternetModel::PlayBehaviour_MultipleItems,
                      InternetModel::Role_PlayBehaviour);
    root_->appendRow(library_);

    favorites_ =
        new QStandardItem(QIcon(":/last.fm/love.png"), tr("Favorites"));
    favorites_->setData(InternetModel::Type_UserPlaylist,
                        InternetModel::Role_Type);
    favorites_->setData(UserFavorites, Role_PlaylistType);
    favorites_->setData(true, InternetModel::Role_CanLazyLoad);
    favorites_->setData(true, InternetModel::Role_CanBeModified);
    favorites_->setData(InternetModel::PlayBehaviour_MultipleItems,
                        InternetModel::Role_PlayBehaviour);
    root_->appendRow(favorites_);

    playlists_parent_ = new QStandardItem(tr("Playlists"));
    root_->appendRow(playlists_parent_);

    subscribed_playlists_parent_ =
        new QStandardItem(tr("Subscribed playlists"));
    root_->appendRow(subscribed_playlists_parent_);

    RetrieveUserFavorites();
    RetrieveUserLibrarySongs();
    RetrieveUserPlaylists();
    RetrieveSubscribedPlaylists();
    RetrieveAutoplayTags();
    RetrievePopularSongs();
  }
}

void GroovesharkService::EnsureConnected() {
  qLog(Debug) << Q_FUNC_INFO;
  EnsureItemsCreated();
}

QStandardItem* GroovesharkService::CreatePlaylistItem(
    const QString& playlist_name, int playlist_id) {
  QStandardItem* item = new QStandardItem(playlist_name);
  item->setData(InternetModel::Type_UserPlaylist, InternetModel::Role_Type);
  item->setData(UserPlaylist, Role_PlaylistType);
  item->setData(true, InternetModel::Role_CanLazyLoad);
  item->setData(true, InternetModel::Role_CanBeModified);
  item->setData(InternetModel::PlayBehaviour_MultipleItems,
                InternetModel::Role_PlayBehaviour);
  item->setData(playlist_id, Role_UserPlaylistId);
  return item;
}

void GroovesharkService::RetrieveUserPlaylists() {
  qLog(Debug) << Q_FUNC_INFO;
  task_playlists_id_ =
      app_->task_manager()->StartTask(tr("Retrieving Grooveshark playlists"));
  GSReply* reply =
      client_->Request("userGetPlaylists",
                       QList<Param>() << Param("userID", client_->getUserID()));

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(UserPlaylistsRetrieved(GSReply*)), reply);
}

void GroovesharkService::UserPlaylistsRetrieved(GSReply* reply) {
  reply->deleteLater();

  QVariantList result = reply->getResult().toMap()["Playlists"].toList();
  QList<PlaylistInfo> playlists = ExtractPlaylistInfo(result);

  for (const PlaylistInfo& playlist_info : playlists) {
    int playlist_id = playlist_info.id_;
    const QString& playlist_name = playlist_info.name_;
    QStandardItem* playlist_item =
        CreatePlaylistItem(playlist_name, playlist_id);
    playlists_parent_->appendRow(playlist_item);

    // Keep in mind this playlist
    playlists_.insert(playlist_id,
                      PlaylistInfo(playlist_id, playlist_name, playlist_item));

    // Request playlist's songs
    RefreshPlaylist(playlist_id);
  }

  if (playlists.isEmpty()) {
    app_->task_manager()->SetTaskFinished(task_playlists_id_);
  }
}

void GroovesharkService::PlaylistSongsRetrieved(GSReply* reply, int playlist_id,
                                                int request_id) {
  reply->deleteLater();

  if (!pending_retrieve_playlists_.remove(request_id)) {
    // This request has been canceled. Stop here
    return;
  }
  PlaylistInfo* playlist_info = subscribed_playlists_.contains(playlist_id)
                                    ? &subscribed_playlists_[playlist_id]
                                    : &playlists_[playlist_id];
  playlist_info->item_->removeRows(0, playlist_info->item_->rowCount());

  QVariantMap result = reply->getResult().toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);
  SortSongsAlphabeticallyIfNeeded(&songs);

  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    child->setData(playlist_info->id_, Role_UserPlaylistId);
    child->setData(true, InternetModel::Role_CanBeModified);

    playlist_info->item_->appendRow(child);
  }

  // Keep in mind this playlist
  playlist_info->songs_ids_ = ExtractSongsIds(result);

  if (pending_retrieve_playlists_.isEmpty()) {
    app_->task_manager()->SetTaskFinished(task_playlists_id_);
  }
}

void GroovesharkService::RetrieveUserFavorites() {
  qLog(Debug) << Q_FUNC_INFO;
  int task_id = app_->task_manager()->StartTask(
      tr("Retrieving Grooveshark favorites songs"));
  //  QNetworkReply* reply = CreateRequestOld("getUserFavoriteSongs",
  //  QList<Param>());
  // playlistGetSongs with htmlshark client
  //  QNetworkReply* reply = CreateRequest("getFavorites", QList<Param>() <<
  //  Param("ofWhat", "Songs") << Param("userID", user_id_));
  GSReply* reply = client_->Request(
      "getFavorites", QList<Param>() << Param("ofWhat", "Songs")
                                     << Param("userID", client_->getUserID()));

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(UserFavoritesRetrieved(GSReply*, int)), reply, task_id);
}

void GroovesharkService::UserFavoritesRetrieved(GSReply* reply, int task_id) {
  reply->deleteLater();
  app_->task_manager()->SetTaskFinished(task_id);

  if (!favorites_) {
    // The use probably logged out before the response arrived.
    return;
  }

  favorites_->removeRows(0, favorites_->rowCount());

  QVariantList result = reply->getResult().toList();
  //  qLog(Debug) << "====\n" << result << "====\n";
  SongList songs = ExtractSongs(QVariantMap(), result, true);
  SortSongsAlphabeticallyIfNeeded(&songs);

  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    child->setData(true, InternetModel::Role_CanBeModified);

    favorites_->appendRow(child);
  }
}

void GroovesharkService::RetrieveUserLibrarySongs() {
  qLog(Debug) << Q_FUNC_INFO;
  int task_id = app_->task_manager()->StartTask(
      tr("Retrieving Grooveshark My Music songs"));
  //  QNetworkReply* reply = CreateRequestOld("getUserLibrarySongs",
  //  QList<Param>());
  GSReply* reply =
      client_->Request("userGetSongsInLibrary",
                       QList<Param>() << Param("userID", client_->getUserID())
                                      << Param("page", 0),
                       true);

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(UserLibrarySongsRetrieved(GSReply*, int)), reply, task_id);
}

void GroovesharkService::UserLibrarySongsRetrieved(GSReply* reply,
                                                   int task_id) {
  reply->deleteLater();
  app_->task_manager()->SetTaskFinished(task_id);

  if (!library_) {
    // The use probably logged out before the response arrived.
    return;
  }

  library_->removeRows(0, library_->rowCount());

  QVariantMap result = reply->getResult().toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);
  SortSongsAlphabeticallyIfNeeded(&songs);

  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    child->setData(true, InternetModel::Role_CanBeModified);

    library_->appendRow(child);
  }
}

void GroovesharkService::RetrievePopularSongs() {
  qLog(Debug) << Q_FUNC_INFO;
  task_popular_id_ =
      app_->task_manager()->StartTask(tr("Getting Grooveshark popular songs"));
  RetrievePopularSongsMonth();
  RetrievePopularSongsToday();
}

void GroovesharkService::RetrievePopularSongsMonth() {
  QList<Param> parameters;
  parameters << Param("type", "monthly");
  //  QNetworkReply* reply = CreateRequestOld("getPopularSongsMonth",
  //  parameters);
  GSReply* reply = client_->Request("popularGetSongs", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PopularSongsMonthRetrieved(GSReply*)), reply);
}

void GroovesharkService::PopularSongsMonthRetrieved(GSReply* reply) {
  reply->deleteLater();
  QVariantMap result = reply->getResult().toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);

  app_->task_manager()->IncreaseTaskProgress(task_popular_id_, 50, 100);
  if (app_->task_manager()->GetTaskProgress(task_popular_id_) >= 100) {
    app_->task_manager()->SetTaskFinished(task_popular_id_);
  }

  if (!popular_month_) return;

  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    popular_month_->appendRow(child);
  }
}

void GroovesharkService::RetrievePopularSongsToday() {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("type", "daily");
  //  QNetworkReply* reply = CreateRequestOld("getPopularSongsToday",
  //  parameters);
  GSReply* reply = client_->Request("popularGetSongs", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PopularSongsTodayRetrieved(GSReply*)), reply);
}

void GroovesharkService::PopularSongsTodayRetrieved(GSReply* reply) {
  reply->deleteLater();
  QVariantMap result = reply->getResult().toMap();
  SongList songs = ExtractSongs(QVariantMap(), result["Songs"].toList(), true);

  app_->task_manager()->IncreaseTaskProgress(task_popular_id_, 50, 100);
  if (app_->task_manager()->GetTaskProgress(task_popular_id_) >= 100) {
    app_->task_manager()->SetTaskFinished(task_popular_id_);
  }

  if (!popular_today_) return;

  for (const Song& song : songs) {
    QStandardItem* child = CreateSongItem(song);
    popular_today_->appendRow(child);
  }
}

void GroovesharkService::RetrieveSubscribedPlaylists() {
  qLog(Debug) << Q_FUNC_INFO;
  GSReply* reply =
      //      CreateRequestOld("getUserPlaylistsSubscribed", QList<Param>());
      client_->Request("getSubscribedPlaylistsBroadcasts", QList<Param>(),
                       true);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SubscribedPlaylistsRetrieved(GSReply*)), reply);
}

void GroovesharkService::SubscribedPlaylistsRetrieved(GSReply* reply) {
  reply->deleteLater();

  QVariantList result = reply->getResult().toMap()["playlists"].toList();
  QList<PlaylistInfo> playlists = ExtractPlaylistInfo(result);

  for (const PlaylistInfo& playlist_info : playlists) {
    int playlist_id = playlist_info.id_;
    const QString& playlist_name = playlist_info.name_;

    QStandardItem* playlist_item =
        CreatePlaylistItem(playlist_name, playlist_id);
    // Refine some playlist properties that should be different for subscribed
    // playlists
    playlist_item->setData(SubscribedPlaylist, Role_PlaylistType);
    playlist_item->setData(false, InternetModel::Role_CanBeModified);

    subscribed_playlists_.insert(
        playlist_id, PlaylistInfo(playlist_id, playlist_name, playlist_item));
    subscribed_playlists_parent_->appendRow(playlist_item);

    // Request playlist's songs
    RefreshPlaylist(playlist_id);
  }
}

void GroovesharkService::RetrieveAutoplayTags() {
  qLog(Debug) << Q_FUNC_INFO;
  GSReply* reply = client_->Request("getTagList", QList<Param>());
  //  GSReply* reply = client_->Request("getTopLevelTags", QList<Param>());
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(AutoplayTagsRetrieved(GSReply*)), reply);
}

void GroovesharkService::AutoplayTagsRetrieved(GSReply* reply) {
  reply->deleteLater();
  //  QVariantList result = reply->getResult().toList();
  QVariantMap result = reply->getResult().toMap();
  //  QVariantMap::const_iterator it;
  if (!stations_) return;
  //  for (it = result.constBegin(); it != result.constEnd(); ++it) {
  //  for (const auto i : result) {
  for (const auto k : result.keys()) {
    //    QVariantMap it = i.toMap();
    //    QString name = it["Tag"].toString().toLower();
    QString name = k.toLower();
    //    int id = it["TagID"].toInt();
    int id = result.value(k).toInt();
    // Names received aren't very nice: make them more user friendly to display
    name.replace("_", " ");
    name[0] = name[0].toUpper();

    QStandardItem* item =
        new QStandardItem(QIcon(":last.fm/icon_radio.png"), name);
    item->setData(InternetModel::Type_SmartPlaylist, InternetModel::Role_Type);
    item->setData(InternetModel::PlayBehaviour_SingleItem,
                  InternetModel::Role_PlayBehaviour);
    item->setData(id, Role_UserPlaylistId);

    stations_->appendRow(item);
  }
}

Song GroovesharkService::StartAutoplayTag(int tag_id,
                                          QVariantMap& autoplay_state) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("tagID", tag_id);
  GSReply* reply = client_->Request("startAutoplayTag", parameters);

  bool reply_has_timeouted = !WaitForGSReply(reply);
  reply->deleteLater();
  if (reply_has_timeouted) return Song();

  QVariantMap result = reply->getResult().toMap();
  autoplay_state = result["autoplayState"].toMap();
  return ExtractSong(result["nextSong"].toMap());
}

Song GroovesharkService::StartAutoplay(QVariantMap& autoplay_state) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  QVariantList artists_ids_qvariant;
  for (int artist_id : last_artists_ids_) {
    artists_ids_qvariant << QVariant(artist_id);
  }
  QVariantList songs_ids_qvariant;
  for (int song_id : last_songs_ids_) {
    songs_ids_qvariant << QVariant(song_id);
  }
  parameters << Param("artistIDs", artists_ids_qvariant)
             << Param("songIDs", songs_ids_qvariant);
  // This used in grooveshark radio
  GSReply* reply = client_->Request("startAutoplay", parameters);

  bool reply_has_timeouted = !WaitForGSReply(reply);
  reply->deleteLater();
  if (reply_has_timeouted) return Song();

  QVariantMap result = reply->getResult().toMap();
  autoplay_state = result["autoplayState"].toMap();
  return ExtractSong(result["nextSong"].toMap());
}

Song GroovesharkService::GetAutoplaySong(QVariantMap& autoplay_state) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("autoplayState", autoplay_state);
  GSReply* reply = client_->Request("getAutoplaySong", parameters);

  bool reply_has_timeouted = !WaitForGSReply(reply);
  reply->deleteLater();
  if (reply_has_timeouted) return Song();

  QVariantMap result = reply->getResult().toMap();
  autoplay_state = result["autoplayState"].toMap();
  return ExtractSong(result["nextSong"].toMap());
}

void GroovesharkService::MarkStreamKeyOver30Secs(const QString& stream_key,
                                                 const QString& server_id,
                                                 const QString& song_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("streamKey", stream_key)
             << Param("streamServerID", server_id.toInt())
             << Param("songID", song_id.toInt());
  //             << Param("artistID", artist_id);

  GSReply* reply = client_->Request("markStreamKeyOver30Seconds", parameters);
  NewClosure(reply, SIGNAL(Finished()), this, SLOT(StreamMarked(GSReply*)),
             reply);
}
// TODO
void GroovesharkService::StreamMarked(GSReply* reply) {
  reply->deleteLater();
  QVariantMap result = reply->getResult().toMap();
  qLog(Debug) << result << "=====markover30Sec=====";
  if (!result["success"].toBool()) {
    qLog(Warning) << "Grooveshark markStreamKeyOver30Secs failed";
  } else {
    qLog(Warning) << "song marked successfully!!!!!!!!!!!!";
  }
}

void GroovesharkService::MarkSongComplete(const QString& song_id,
                                          const QString& stream_key,
                                          const QString& server_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("songID", song_id.toInt())
             << Param("streamKey", stream_key)
             << Param("streamServerID", server_id.toInt());

  GSReply* reply = client_->Request("markSongComplete", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SongMarkedAsComplete(GSReply*)), reply);
}

void GroovesharkService::SongMarkedAsComplete(GSReply* reply) {
  reply->deleteLater();
  QVariantMap result = reply->getResult().toMap();
  qLog(Debug) << "------------markSongComplete------------";
  //  if (!result["success"].toBool()) {
  //    qLog(Warning) << "Grooveshark markSongComplete failed";
  //  }
}

void GroovesharkService::ItemDoubleClicked(QStandardItem* item) {
  if (item == root_) {
    EnsureConnected();
  }
}

GeneratorPtr GroovesharkService::CreateGenerator(QStandardItem* item) {
  GeneratorPtr ret;
  if (!item ||
      item->data(InternetModel::Role_Type).toInt() !=
          InternetModel::Type_SmartPlaylist) {
    return ret;
  }

  if (item == grooveshark_radio_) {
    if (last_artists_ids_.isEmpty()) {
      QMessageBox::warning(nullptr, tr("Error"),
                           tr("To start Grooveshark radio, you should first "
                              "listen to a few other Grooveshark songs"));
      return ret;
    }
    ret = GeneratorPtr(new GroovesharkRadio(this));
  } else {
    int tag_id = item->data(Role_UserPlaylistId).toInt();
    ret = GeneratorPtr(new GroovesharkRadio(this, tag_id));
  }
  return ret;
}

void GroovesharkService::DropMimeData(const QMimeData* data,
                                      const QModelIndex& index) {
  if (!data) {
    return;
  }

  // Get Grooveshark songs' ids, if any.
  QList<int> data_songs_ids = ExtractSongsIds(data->urls());
  if (data_songs_ids.isEmpty()) {
    // There is none: probably means user didn't dropped Grooveshark songs
    return;
  }

  int type = index.data(InternetModel::Role_Type).toInt();
  int parent_type = index.parent().data(InternetModel::Role_Type).toInt();

  if (type == InternetModel::Type_UserPlaylist ||
      parent_type == InternetModel::Type_UserPlaylist) {
    int playlist_type = index.data(Role_PlaylistType).toInt();
    int parent_playlist_type = index.parent().data(Role_PlaylistType).toInt();
    // If dropped on Favorites list
    if (playlist_type == UserFavorites ||
        parent_playlist_type == UserFavorites) {
      for (int song_id : data_songs_ids) {
        AddUserFavoriteSong(song_id);
      }
    } else if (playlist_type == UserLibrary ||
               parent_playlist_type == UserLibrary) {
      AddUserLibrarySongs(ExtractSongs(data->urls()));
    } else {  // Dropped on a normal playlist
      // Get the playlist
      int playlist_id = index.data(Role_UserPlaylistId).toInt();
      if (!playlists_.contains(playlist_id)) {
        return;
      }
      // Get the current playlist's songs
      PlaylistInfo playlist = playlists_[playlist_id];
      QList<int> songs_ids = playlist.songs_ids_;
      songs_ids << data_songs_ids;

      SetPlaylistSongs(playlist_id, songs_ids);
    }
  }
}

QList<QAction*> GroovesharkService::playlistitem_actions(const Song& song) {
  // Clear previous actions
  while (!playlistitem_actions_.isEmpty()) {
    QAction* action = playlistitem_actions_.takeFirst();
    delete action->menu();
    delete action;
  }

  // Create a 'add to favorites' action
  QAction* add_to_favorites = new QAction(
      QIcon(":/last.fm/love.png"), tr("Add to Grooveshark favorites"), this);
  connect(add_to_favorites, SIGNAL(triggered()),
          SLOT(AddCurrentSongToUserFavorites()));
  playlistitem_actions_.append(add_to_favorites);

  QAction* add_to_library =
      new QAction(IconLoader::Load("folder-sound"),
                  tr("Add to Grooveshark My Music"), this);
  connect(add_to_library, SIGNAL(triggered()),
          SLOT(AddCurrentSongToUserLibrary()));
  playlistitem_actions_.append(add_to_library);

  // Create a menu with 'add to playlist' actions for each Grooveshark playlist
  QAction* add_to_playlists = new QAction(
      IconLoader::Load("list-add"), tr("Add to Grooveshark playlists"), this);
  QMenu* playlists_menu = new QMenu();
  for (const PlaylistInfo& playlist_info : playlists_.values()) {
    QAction* add_to_playlist = new QAction(playlist_info.name_, this);
    add_to_playlist->setData(playlist_info.id_);
    playlists_menu->addAction(add_to_playlist);
  }
  connect(playlists_menu, SIGNAL(triggered(QAction*)),
          SLOT(AddCurrentSongToPlaylist(QAction*)));
  add_to_playlists->setMenu(playlists_menu);
  playlistitem_actions_.append(add_to_playlists);

  QAction* share_song =
      new QAction(tr("Get a URL to share this Grooveshark song"), this);
  connect(share_song, SIGNAL(triggered()), SLOT(GetCurrentSongUrlToShare()));
  playlistitem_actions_.append(share_song);

  // Keep in mind the current song id
  //  current_song_id_ = ExtractSongId(song.url());
  current_song_info_ = ExtractSongInfoFromUrl(song.url());

  return playlistitem_actions_;
}

void GroovesharkService::GetCurrentSongUrlToShare() {
  GetSongUrlToShare(current_song_info_["songID"].toInt());
}

void GroovesharkService::GetSongUrlToShare(int song_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("songID", song_id);
  // TODO
  // currently it is not work
  GSReply* reply = client_->Request("getSongURLFromSongID", parameters);

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SongUrlToShareReceived(GSReply*)), reply);
}

void GroovesharkService::SongUrlToShareReceived(GSReply* reply) {
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  if (!result["url"].isValid()) return;
  QString url = result["url"].toString();
  ShowUrlBox(tr("Grooveshark song's URL"), url);
}

void GroovesharkService::GetCurrentPlaylistUrlToShare() {
  GetPlaylistUrlToShare(current_playlist_id_);
}

void GroovesharkService::GetPlaylistUrlToShare(int playlist_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("playlistID", playlist_id);
  // TODO
  // currently it is not work
  GSReply* reply = client_->Request("getPlaylistURLFromPlaylistID", parameters);

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PlaylistUrlToShareReceived(GSReply*)), reply);
}

void GroovesharkService::PlaylistUrlToShareReceived(GSReply* reply) {
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  if (!result["url"].isValid()) return;
  QString url = result["url"].toString();
  ShowUrlBox(tr("Grooveshark playlist's URL"), url);
}

void GroovesharkService::AddCurrentSongToPlaylist(QAction* action) {
  int playlist_id = action->data().toInt();
  if (!playlists_.contains(playlist_id)) {
    return;
  }
  // Get the current playlist's songs
  PlaylistInfo playlist = playlists_[playlist_id];
  QList<int> songs_ids = playlist.songs_ids_;
  songs_ids << current_song_info_["songID"].toInt();

  SetPlaylistSongs(playlist_id, songs_ids);
}

void GroovesharkService::SetPlaylistSongs(int playlist_id,
                                          const QList<int>& songs_ids) {
  qLog(Debug) << Q_FUNC_INFO;
  // If we are still retrieving playlists songs, don't update playlist: don't
  // take the risk to erase all (not yet retrieved) playlist's songs.
  if (!pending_retrieve_playlists_.isEmpty()) return;
  int task_id =
      app_->task_manager()->StartTask(tr("Update Grooveshark playlist"));

  QList<Param> parameters;

  // Convert song ids to QVariant
  QVariantList songs_ids_qvariant;
  for (int song_id : songs_ids) {
    songs_ids_qvariant << QVariant(song_id);
  }

  parameters << Param("playlistID", playlist_id)
             << Param("songIDs", songs_ids_qvariant);

  GSReply* reply = client_->Request("overwritePlaylistEx", parameters);

  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PlaylistSongsSet(GSReply*, int, int)), reply, playlist_id,
             task_id);
}

void GroovesharkService::PlaylistSongsSet(GSReply* reply, int playlist_id,
                                          int task_id) {
  reply->deleteLater();

  app_->task_manager()->SetTaskFinished(task_id);

  int result = reply->getResult().toInt();
  if (result == 0) {
    qLog(Warning) << "Grooveshark setPlaylistSongs failed";
    return;
  }

  RefreshPlaylist(playlist_id);
}

void GroovesharkService::RefreshPlaylist(int playlist_id) {
  qLog(Debug) << Q_FUNC_INFO;
  QList<Param> parameters;
  parameters << Param("playlistID", playlist_id);
  //  QNetworkReply* reply = CreateRequestOld("getPlaylistSongs", parameters);
  GSReply* reply = client_->Request("getPlaylistByID", parameters);
  int id = next_pending_playlist_retrieve_id_++;
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PlaylistSongsRetrieved(GSReply*, int, int)), reply,
             playlist_id, id);

  pending_retrieve_playlists_.insert(id);
}

void GroovesharkService::CreateNewPlaylist() {
  qLog(Debug) << Q_FUNC_INFO;
  QString name =
      QInputDialog::getText(nullptr, tr("Create a new Grooveshark playlist"),
                            tr("Name"), QLineEdit::Normal);
  if (name.isEmpty()) {
    return;
  }

  QList<Param> parameters;
  parameters << Param("playlistName", name) << Param("songIDs", QVariantList());
  GSReply* reply = client_->Request("createPlaylistEx", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(NewPlaylistCreated(GSReply*, const QString&)), reply, name);
}

void GroovesharkService::NewPlaylistCreated(GSReply* reply,
                                            const QString& name) {
  reply->deleteLater();
  QVariant result = reply->getResult();

  if (result.isNull()) {
    qLog(Warning) << "Grooveshark createPlaylist failed";
    return;
  }

  int playlist_id = result.toInt();
  QStandardItem* new_playlist_item = CreatePlaylistItem(name, playlist_id);
  PlaylistInfo playlist_info(playlist_id, name, new_playlist_item);
  playlist_info.item_ = new_playlist_item;
  playlists_parent_->appendRow(new_playlist_item);
  playlists_.insert(playlist_id, playlist_info);
}

void GroovesharkService::DeleteCurrentPlaylist() {
  qLog(Debug) << Q_FUNC_INFO;
  if (model()->current_index().data(InternetModel::Role_Type).toInt() !=
      InternetModel::Type_UserPlaylist) {
    return;
  }

  int playlist_id = model()->current_index().data(Role_UserPlaylistId).toInt();
  DeletePlaylist(playlist_id);
}

void GroovesharkService::DeletePlaylist(int playlist_id) {
  if (!playlists_.contains(playlist_id)) {
    return;
  }

  std::unique_ptr<QMessageBox> confirmation_dialog(
      new QMessageBox(QMessageBox::Question, tr("Delete Grooveshark playlist"),
                      tr("Are you sure you want to delete this playlist?"),
                      QMessageBox::Yes | QMessageBox::Cancel));
  if (confirmation_dialog->exec() != QMessageBox::Yes) {
    return;
  }

  QList<Param> parameters;
  parameters << Param("playlistID", playlist_id)
             << Param("name", playlists_[playlist_id].name_);
  GSReply* reply = client_->Request("deletePlaylist", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PlaylistDeleted(GSReply*, int)), reply, playlist_id);
}

void GroovesharkService::PlaylistDeleted(GSReply* reply, int playlist_id) {
  reply->deleteLater();
  int result = reply->getResult().toInt();
  if (result == 0) {
    qLog(Warning) << "Grooveshark deletePlaylist failed";
    return;
  }
  if (!playlists_.contains(playlist_id)) {
    return;
  }
  PlaylistInfo playlist_info = playlists_.take(playlist_id);
  playlists_parent_->removeRow(playlist_info.item_->row());
}

void GroovesharkService::RenameCurrentPlaylist() {
  qLog(Debug) << Q_FUNC_INFO;
  const QModelIndex& index(model()->current_index());

  if (index.data(InternetModel::Role_Type).toInt() !=
          InternetModel::Type_UserPlaylist ||
      index.data(Role_PlaylistType).toInt() != UserPlaylist) {
    return;
  }

  const int playlist_id = index.data(Role_UserPlaylistId).toInt();
  RenamePlaylist(playlist_id);
}

void GroovesharkService::RenamePlaylist(int playlist_id) {
  if (!playlists_.contains(playlist_id)) {
    return;
  }
  const QString& old_name = playlists_[playlist_id].name_;
  QString new_name =
      QInputDialog::getText(nullptr, tr("Rename \"%1\" playlist").arg(old_name),
                            tr("Name"), QLineEdit::Normal, old_name);
  if (new_name.isEmpty()) {
    return;
  }

  QList<Param> parameters;
  parameters << Param("playlistID", playlist_id)
             << Param("playlistName", new_name);
  GSReply* reply = client_->Request("renamePlaylist", parameters);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(PlaylistRenamed(GSReply*, int, const QString&)), reply,
             playlist_id, new_name);
}

void GroovesharkService::PlaylistRenamed(GSReply* reply, int playlist_id,
                                         const QString& new_name) {
  reply->deleteLater();
  bool result = reply->getResult().toBool();
  if (!result) {
    qLog(Warning) << "Grooveshark renamePlaylist failed";
    return;
  }
  if (!playlists_.contains(playlist_id)) {
    return;
  }
  PlaylistInfo& playlist_info = playlists_[playlist_id];
  playlist_info.name_ = new_name;
  playlist_info.item_->setText(new_name);
}

void GroovesharkService::AddUserFavoriteSong(int song_id) {
  qLog(Debug) << Q_FUNC_INFO;
  int task_id = app_->task_manager()->StartTask(tr("Adding song to favorites"));
  QList<Param> parameters;
  parameters << Param("ID", song_id) << Param("what", "Song");
  GSReply* reply = client_->Request("favorite", parameters, true);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(UserFavoriteSongAdded(GSReply*, int)), reply, task_id);
}

void GroovesharkService::UserFavoriteSongAdded(GSReply* reply, int task_id) {
  reply->deleteLater();
  app_->task_manager()->SetTaskFinished(task_id);

  QVariantMap result = reply->getResult().toMap();
  if (!result["success"].toBool()) {
    qLog(Warning) << "Grooveshark addUserFavoriteSong failed";
    return;
  }
  // Refresh user's favorites list
  RetrieveUserFavorites();
}

void GroovesharkService::AddUserLibrarySongs(const QVariantList& songs) {
  qLog(Debug) << Q_FUNC_INFO;
  int task_id = app_->task_manager()->StartTask(tr("Adding song to My Music"));
  QList<Param> parameters;

  parameters << Param("songs", songs);
  GSReply* reply = client_->Request("userAddSongsToLibrary", parameters, true);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(UserLibrarySongAdded(GSReply*, int)), reply, task_id);
}

void GroovesharkService::UserLibrarySongAdded(GSReply* reply, int task_id) {
  reply->deleteLater();

  app_->task_manager()->SetTaskFinished(task_id);

  //  QVariantMap result = reply->getResult().toMap();
  //  if (!result["success"].toBool()) {
  //    qLog(Warning) << "Grooveshark addUserLibrarySongs failed";
  //    return;
  //  }
  // Refresh user's library list
  RetrieveUserLibrarySongs();
}

void GroovesharkService::RemoveCurrentFromPlaylist() {
  const QModelIndexList& indexes(model()->selected_indexes());
  QMap<int, QList<int>> playlists_songs_ids;
  for (const QModelIndex& index : indexes) {
    if (index.parent().data(InternetModel::Role_Type).toInt() !=
        InternetModel::Type_UserPlaylist) {
      continue;
    }

    int playlist_id = index.data(Role_UserPlaylistId).toInt();
    int song_id = ExtractSongId(index.data(InternetModel::Role_Url).toUrl());
    if (song_id) {
      playlists_songs_ids[playlist_id] << song_id;
    }
  }

  for (QMap<int, QList<int>>::const_iterator it =
           playlists_songs_ids.constBegin();
       it != playlists_songs_ids.constEnd(); ++it) {
    RemoveFromPlaylist(it.key(), it.value());
  }
}

void GroovesharkService::RemoveFromPlaylist(
    int playlist_id, const QList<int>& songs_ids_to_remove) {
  qLog(Debug) << Q_FUNC_INFO;
  if (!playlists_.contains(playlist_id)) {
    return;
  }

  QList<int> songs_ids = playlists_[playlist_id].songs_ids_;
  for (const int song_id : songs_ids_to_remove) {
    songs_ids.removeOne(song_id);
  }

  SetPlaylistSongs(playlist_id, songs_ids);
}

void GroovesharkService::RemoveCurrentFromFavorites() {
  const QModelIndexList& indexes(model()->selected_indexes());
  //  QList<int> songs_ids;
  for (const QModelIndex& index : indexes) {
    if (index.parent().data(Role_PlaylistType).toInt() != UserFavorites) {
      continue;
    }

    int song_id = ExtractSongId(index.data(InternetModel::Role_Url).toUrl());
    if (song_id) {
      //      songs_ids << song_id;
      RemoveFromFavorites(song_id);
    }
  }

  //  RemoveFromFavorites(songs_ids);
}

void GroovesharkService::RemoveFromFavorites(int song_id) {
  qLog(Debug) << Q_FUNC_INFO;
  int task_id =
      app_->task_manager()->StartTask(tr("Removing song from favorites"));
  QList<Param> parameters;

  parameters << Param("what", "Song") << Param("ID", song_id);
  GSReply* reply = client_->Request("unfavorite", parameters, true);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SongsRemovedFromFavorites(GSReply*, int)), reply, task_id);
}

void GroovesharkService::SongsRemovedFromFavorites(GSReply* reply,
                                                   int task_id) {
  app_->task_manager()->SetTaskFinished(task_id);
  reply->deleteLater();

  QVariantMap result = reply->getResult().toMap();
  if (!result["success"].toBool()) {
    qLog(Warning) << "Grooveshark removeUserFavoriteSongs failed";
    return;
  }
  RetrieveUserFavorites();
}

void GroovesharkService::RemoveCurrentFromLibrary() {
  const QModelIndexList& indexes(model()->selected_indexes());
  QList<int> songs_ids;

  for (const QModelIndex& index : indexes) {
    if (index.parent().data(Role_PlaylistType).toInt() != UserLibrary) {
      continue;
    }

    int song_id = ExtractSongId(index.data(InternetModel::Role_Url).toUrl());
    if (song_id) {
      songs_ids << song_id;
    }
  }

  RemoveFromLibrary(songs_ids);
}

void GroovesharkService::RemoveFromLibrary(
    const QList<int>& songs_ids_to_remove) {
  qLog(Debug) << Q_FUNC_INFO;
  if (songs_ids_to_remove.isEmpty()) return;

  int task_id =
      app_->task_manager()->StartTask(tr("Removing songs from My Music"));
  QList<Param> parameters;

  // Convert song ids to QVariant
  QVariantList songs_ids_qvariant;
  for (const int song_id : songs_ids_to_remove) {
    songs_ids_qvariant << QVariant(song_id);
  }
  QVariantList albums_ids_qvariant;
  QVariantList artists_ids_qvariant;

  parameters << Param("songIDs", songs_ids_qvariant)
             << Param("userID", client_->getUserID());
  // We do not support albums and artist parameters for now, but they are
  // required
  parameters << Param("albumIDs", albums_ids_qvariant);
  parameters << Param("artistIDs", artists_ids_qvariant);
  GSReply* reply =
      client_->Request("userRemoveSongsFromLibrary", parameters, true);
  NewClosure(reply, SIGNAL(Finished()), this,
             SLOT(SongsRemovedFromLibrary(GSReply*, int)), reply, task_id);
}

void GroovesharkService::SongsRemovedFromLibrary(GSReply* reply, int task_id) {
  app_->task_manager()->SetTaskFinished(task_id);
  reply->deleteLater();

  //  QVariantMap result = reply->getResult().toMap();
  //  if (!result["success"].toBool()) {
  //    qLog(Warning) << "Grooveshark removeUserLibrarySongs failed";
  //    return;
  //  }
  RetrieveUserLibrarySongs();
}

void GroovesharkService::RequestSslErrors(const QList<QSslError>& errors) {
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

  for (const QSslError& error : errors) {
    emit StreamError("SSL error occurred in Grooveshark request for " +
                     reply->url().toString() + ": " + error.errorString());
  }
}

bool GroovesharkService::WaitForGSReply(GSReply* reply) {
  QEventLoop event_loop;
  QTimer timeout_timer;
  connect(&timeout_timer, SIGNAL(timeout()), &event_loop, SLOT(quit()));
  connect(reply, SIGNAL(Finished()), &event_loop, SLOT(quit()));
  timeout_timer.start(10000);
  event_loop.exec();
  if (!timeout_timer.isActive()) {
    qLog(Error) << "Grooveshark request timeout";
    return false;
  }
  timeout_timer.stop();
  return true;
}

bool GroovesharkService::WaitForReply(QNetworkReply* reply) {
  QEventLoop event_loop;
  QTimer timeout_timer;
  connect(&timeout_timer, SIGNAL(timeout()), &event_loop, SLOT(quit()));
  connect(reply, SIGNAL(finished()), &event_loop, SLOT(quit()));
  timeout_timer.start(10000);
  event_loop.exec();
  if (!timeout_timer.isActive()) {
    qLog(Error) << "Grooveshark request timeout";
    return false;
  }
  timeout_timer.stop();
  return true;
}

namespace {
bool CompareSongs(const QVariant& song1, const QVariant& song2) {
  QMap<QString, QVariant> song1_map = song1.toMap();
  QMap<QString, QVariant> song2_map = song2.toMap();
  int song1_sort = song1_map["Sort"].toInt();
  int song2_sort = song2_map["Sort"].toInt();
  if (song1_sort == song2_sort) {
    // Favorite songs have a "TSFavorited" and (currently) no "Sort" field
    return song1_map["TSFavorited"].toString() <
           song2_map["TSFavorited"].toString();
  }
  return song1_sort < song2_sort;
}
}  // namespace

SongList GroovesharkService::ExtractSongs(const QVariantMap& result_old,
                                          QVariantList result_songs, bool f) {
  //  QVariantList result_songs = result["songs"].toList();
  if (!f) return SongList();
  qStableSort(result_songs.begin(), result_songs.end(), CompareSongs);
  SongList songs;
  for (int i = 0; i < result_songs.size(); ++i) {
    QVariantMap result_song = result_songs[i].toMap();
    songs << ExtractSong(result_song);
  }
  return songs;
}

Song GroovesharkService::ExtractSong(const QVariantMap& result_song) {
  Song song;
  if (!result_song.isEmpty()) {
    int song_id = result_song["SongID"].toInt();
    QString song_name;
    if (result_song.contains("SongName")) {
      song_name = result_song["SongName"].toString();
    } else {
      song_name = result_song["Name"].toString();
    }
    int artist_id = result_song["ArtistID"].toInt();
    QString artist_name = result_song["ArtistName"].toString();
    int album_id = result_song["AlbumID"].toInt();
    QString album_name = result_song["AlbumName"].toString();
    qint64 duration = result_song["EstimateDuration"].toInt() * kNsecPerSec;
    song.Init(song_name, artist_name, album_name, duration);
    QVariant cover = result_song["CoverArtFilename"];
    if (cover.isValid()) {
      song.set_art_automatic(QString(kUrlCover) + cover.toString());
    }
    QVariant track_number = result_song["TrackNum"];
    if (track_number.isValid()) {
      song.set_track(track_number.toInt());
    }
    QVariant year = result_song["Year"];
    if (year.isValid()) {
      song.set_year(year.toInt());
    }
    // Special kind of URL: because we need to request a stream key for each
    // play, we generate a fake URL for now, and we will create a real streaming
    // URL when user will actually play the song (through url handler)
    // URL is grooveshark://artist_id/album_id/song_id
    song.set_url(
        QUrl(QString("grooveshark://%1/%2/%3").arg(artist_id).arg(album_id).arg(
            song_id)));
  }
  return song;
}

QList<int> GroovesharkService::ExtractSongsIds(const QVariantMap& result) {
  QVariantList result_songs = result["Songs"].toList();
  QList<int> songs_ids;
  for (int i = 0; i < result_songs.size(); ++i) {
    QVariantMap result_song = result_songs[i].toMap();
    int song_id = result_song["SongID"].toInt();
    songs_ids << song_id;
  }
  return songs_ids;
}

QList<int> GroovesharkService::ExtractSongsIds(const QList<QUrl>& urls) {
  QList<int> songs_ids;
  for (const QUrl& url : urls) {
    int song_id = ExtractSongId(url);
    if (song_id) {
      songs_ids << song_id;
    }
  }
  return songs_ids;
}

QVariantList GroovesharkService::ExtractSongs(const QList<QUrl>& urls) {
  QVariantList songs;
  for (const QUrl& url : urls) {
    if (url.scheme() == "grooveshark") {
      QVariantMap song = ExtractSongInfoFromUrl(url);
      if (!song.isEmpty()) songs << song;
    }
  }
  return songs;
}

QVariantMap GroovesharkService::ExtractSongInfoFromUrl(const QUrl& url) {
  QVariantMap song;
  if (url.scheme() == "grooveshark") {
    QStringList song_info = url.toString().remove("grooveshark://").split("/");
    if (song_info.size() == 3) {
      song.insert("songID", song_info[2]);
      song.insert("artistID", song_info[0]);
      song.insert("albumID", song_info[1]);
    }
  }
  return song;
}

int GroovesharkService::ExtractSongId(const QUrl& url) {
  if (url.scheme() == "grooveshark") {
    QStringList ids = url.toString().remove("grooveshark://").split("/");
    if (ids.size() == 3)
      // Returns the third id: song id
      return ids[2].toInt();
  }
  return 0;
}

QList<GroovesharkService::PlaylistInfo> GroovesharkService::ExtractPlaylistInfo(
    const QVariantList& playlists_qvariant) {
  QList<PlaylistInfo> playlists;

  // Get playlists info
  for (const QVariant& playlist_qvariant : playlists_qvariant) {
    QVariantMap playlist = playlist_qvariant.toMap();
    int playlist_id = playlist["PlaylistID"].toInt();
    QString playlist_name = playlist["Name"].toString();

    playlists << PlaylistInfo(playlist_id, playlist_name);
  }

  // Sort playlists by name
  qSort(playlists.begin(), playlists.end());

  return playlists;
}

void GroovesharkService::SortSongsAlphabeticallyIfNeeded(
    SongList* songs) const {
  QSettings s;
  s.beginGroup(GroovesharkService::kSettingsGroup);
  const bool sort_songs_alphabetically =
      s.value("sort_alphabetically").toBool();
  if (sort_songs_alphabetically) {
    Song::SortSongsListAlphabetically(songs);
  }
}
