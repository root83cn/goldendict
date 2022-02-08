/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "articleview.hh"
#include <map>
#include <QMessageBox>
#include "weburlrequestinterceptor.h"
#include <QMenu>
#include <QDesktopServices>
#include <QWebEngineHistory>
#include <QWebEngineScript>
#include <QWebEngineSettings>
#include <QWebEngineScriptCollection>
#include <QClipboard>
#include <QKeyEvent>
#include <QFileDialog>
#include "folding.hh"
#include "wstring_qt.hh"
#include "webmultimediadownload.hh"
#include "programs.hh"
#include "gddebug.hh"
#include <QDebug>
#include <QCryptographicHash>
#include "gestures.hh"
#include "fulltextsearch.hh"
#include <QWebChannel>

#if QT_VERSION >= QT_VERSION_CHECK( 5, 0, 0 )
#include <QRegularExpression>
#include "wildcard.hh"
#endif

#include "qt4x5.hh"

#include <assert.h>
#include <map>
#include <QWebEngineContextMenuData>
#ifdef Q_OS_WIN32
#include <windows.h>
#include <QPainter>
#endif

#include <QBuffer>

#if defined( Q_OS_WIN32 ) || defined( Q_OS_MAC )
#include "speechclient.hh"
#endif

#include "globalbroadcaster.h"
using std::map;
using std::list;

/// AccentMarkHandler class
///
/// Remove accent marks from text
/// and mirror position in normalized text to original text

class AccentMarkHandler
{
protected:
  QString normalizedString;
  QVector< int > accentMarkPos;
public:
  AccentMarkHandler()
  {}
  virtual ~AccentMarkHandler()
  {}
  static QChar accentMark()
  { return QChar( 0x301 ); }

  /// Create text without accent marks
  /// and store mark positions
  virtual void setText( QString const & baseString )
  {
    accentMarkPos.clear();
    normalizedString.clear();
    int pos = 0;
    QChar mark = accentMark();

    for( int x = 0; x < baseString.length(); x++ )
    {
      if( baseString.at( x ) == mark )
      {
        accentMarkPos.append( pos );
        continue;
      }
      normalizedString.append( baseString.at( x ) );
      pos++;
    }
  }

  /// Return text without accent marks
  QString const & normalizedText() const
  { return normalizedString; }

  /// Convert position into position in original text
  int mirrorPosition( int const & pos ) const
  {
    int newPos = pos;
    for( int x = 0; x < accentMarkPos.size(); x++ )
    {
      if( accentMarkPos.at( x ) < pos )
        newPos++;
      else
        break;
    }
    return newPos;
  }
};

/// End of DslAccentMark class

/// DiacriticsHandler class
///
/// Remove diacritics from text
/// and mirror position in normalized text to original text

class DiacriticsHandler : public AccentMarkHandler
{
public:
  DiacriticsHandler()
  {}
  ~DiacriticsHandler()
  {}

  /// Create text without diacriticss
  /// and store diacritic marks positions
  virtual void setText( QString const & baseString )
  {
    accentMarkPos.clear();
    normalizedString.clear();

    gd::wstring baseText = gd::toWString( baseString );
    gd::wstring normText;

    int pos = 0;
    normText.reserve( baseText.size() );

    gd::wchar const * nextChar = baseText.data();
    size_t consumed;

    for( size_t left = baseText.size(); left; )
    {
      if( *nextChar >= 0x10000U )
      {
        // Will be translated into surrogate pair
        normText.push_back( *nextChar );
        pos += 2;
        nextChar++; left--;
        continue;
      }

      gd::wchar ch = Folding::foldedDiacritic( nextChar, left, consumed );

      if( Folding::isCombiningMark( ch ) )
      {
        accentMarkPos.append( pos );
        nextChar++; left--;
        continue;
      }

      if( consumed > 1 )
      {
        for( size_t i = 1; i < consumed; i++ )
          accentMarkPos.append( pos );
      }

      normText.push_back( ch );
      pos += 1;
      nextChar += consumed;
      left -= consumed;
    }
    normalizedString = gd::toQString( normText );
  }
};
/// End of DiacriticsHandler class

void ArticleView::emitJavascriptFinished(){
  emit notifyJavascriptFinished();
}
//in webengine,javascript has been executed in async mode ,for simpility,use EventLoop to simulate sync execution.
//a better solution would be to replace it with callback etc.
QString ArticleView::runJavaScriptSync(QWebEnginePage *frame, const QString &variable)
{
  qDebug("%s", QString("runJavascriptScriptSync:%1").arg(variable).toLatin1().data());

  QString result;
  QSharedPointer<QEventLoop> loop = QSharedPointer<QEventLoop>(new QEventLoop());
  QTimer::singleShot(1000, loop.data(), &QEventLoop::quit);
  frame->runJavaScript(variable, [=, &result](const QVariant &v)
      {
        if(loop->isRunning()){
            if(v.isValid())
                result = v.toString();
            loop->quit();
        } 
      });

  loop->exec();
  return result;
}

namespace {

char const * const scrollToPrefix = "gdfrom-";

bool isScrollTo( QString const & id )
{
  return id.startsWith( scrollToPrefix );
}

QString dictionaryIdFromScrollTo( QString const & scrollTo )
{
  Q_ASSERT( isScrollTo( scrollTo ) );
  const int scrollToPrefixLength = 7;
  return scrollTo.mid( scrollToPrefixLength );
}

} // unnamed namespace

QString ArticleView::scrollToFromDictionaryId( QString const & dictionaryId )
{
  Q_ASSERT( !isScrollTo( dictionaryId ) );
  return scrollToPrefix + dictionaryId;
}

ArticleView::ArticleView( QWidget * parent, ArticleNetworkAccessManager & nm,
                          AudioPlayerPtr const & audioPlayer_,
                          std::vector< sptr< Dictionary::Class > > const & allDictionaries_,
                          Instances::Groups const & groups_, bool popupView_,
                          Config::Class const & cfg_,
                          QAction & openSearchAction_,
                          QAction * dictionaryBarToggled_,
                          GroupComboBox const * groupComboBox_ ):
  QFrame( parent ),
  articleNetMgr( nm ),
  audioPlayer( audioPlayer_ ),
  allDictionaries( allDictionaries_ ),
  groups( groups_ ),
  popupView( popupView_ ),
  cfg( cfg_ ),
  pasteAction( this ),
  articleUpAction( this ),
  articleDownAction( this ),
  goBackAction( this ),
  goForwardAction( this ),
  selectCurrentArticleAction( this ),
  copyAsTextAction( this ),
  inspectAction( this ),
  openSearchAction( openSearchAction_ ),
  searchIsOpened( false ),
  dictionaryBarToggled( dictionaryBarToggled_ ),
  groupComboBox( groupComboBox_ ),
  ftsSearchIsOpened( false ),
  ftsSearchMatchCase( false ),
  ftsPosition( 0 )
{
  ui.setupUi( this );

  ui.definition->setUp( const_cast< Config::Class * >( &cfg ) );

  goBackAction.setShortcut( QKeySequence( "Alt+Left" ) );
  ui.definition->addAction( &goBackAction );
  connect( &goBackAction, SIGNAL( triggered() ),
           this, SLOT( back() ) );

  goForwardAction.setShortcut( QKeySequence( "Alt+Right" ) );
  ui.definition->addAction( &goForwardAction );
  connect( &goForwardAction, SIGNAL( triggered() ),
           this, SLOT( forward() ) );

  ui.definition->pageAction( QWebEnginePage::Copy )->setShortcut( QKeySequence::Copy );
  ui.definition->addAction( ui.definition->pageAction( QWebEnginePage::Copy ) );

  QAction * selectAll = ui.definition->pageAction( QWebEnginePage::SelectAll );
  selectAll->setShortcut( QKeySequence::SelectAll );
  selectAll->setShortcutContext( Qt::WidgetWithChildrenShortcut );
  ui.definition->addAction( selectAll );

  ui.definition->setContextMenuPolicy( Qt::CustomContextMenu );

  connect(ui.definition, SIGNAL(loadFinished(bool)), this,
          SLOT(loadFinished(bool)));

  connect( ui.definition->page(), SIGNAL( titleChanged( QString const & ) ),
           this, SLOT( handleTitleChanged( QString const & ) ) );

  connect( ui.definition->page(), SIGNAL( urlChanged(QUrl) ),
           this, SLOT( handleUrlChanged(QUrl) ) );

  connect( ui.definition, SIGNAL( customContextMenuRequested( QPoint const & ) ),
           this, SLOT( contextMenuRequested( QPoint const & ) ) );

  connect( ui.definition->page(), SIGNAL( linkHovered ( const QString &) ),
           this, SLOT( linkHovered ( const QString & ) ) );

  connect( ui.definition, SIGNAL( doubleClicked( QPoint ) ),this,SLOT( doubleClicked( QPoint ) ) );

  pasteAction.setShortcut( QKeySequence::Paste  );
  ui.definition->addAction( &pasteAction );
  connect( &pasteAction, SIGNAL( triggered() ), this, SLOT( pasteTriggered() ) );

  articleUpAction.setShortcut( QKeySequence( "Alt+Up" ) );
  ui.definition->addAction( &articleUpAction );
  connect( &articleUpAction, SIGNAL( triggered() ), this, SLOT( moveOneArticleUp() ) );

  articleDownAction.setShortcut( QKeySequence( "Alt+Down" ) );
  ui.definition->addAction( &articleDownAction );
  connect( &articleDownAction, SIGNAL( triggered() ), this, SLOT( moveOneArticleDown() ) );

  ui.definition->addAction( &openSearchAction );
  connect( &openSearchAction, SIGNAL( triggered() ), this, SLOT( openSearch() ) );

  selectCurrentArticleAction.setShortcut( QKeySequence( "Ctrl+Shift+A" ));
  selectCurrentArticleAction.setText( tr( "Select Current Article" ) );
  ui.definition->addAction( &selectCurrentArticleAction );
  connect( &selectCurrentArticleAction, SIGNAL( triggered() ),
           this, SLOT( selectCurrentArticle() ) );

  copyAsTextAction.setShortcut( QKeySequence( "Ctrl+Shift+C" ) );
  copyAsTextAction.setText( tr( "Copy as text" ) );
  ui.definition->addAction( &copyAsTextAction );
  connect( &copyAsTextAction, SIGNAL( triggered() ),
           this, SLOT( copyAsText() ) );

  inspectAction.setShortcut( QKeySequence( Qt::Key_F12 ) );
  inspectAction.setText( tr( "Inspect" ) );
  ui.definition->addAction( &inspectAction );
  //connect( &inspectAction, SIGNAL( triggered() ), this, SLOT( inspect() ) );

  QWebEnginePage *page = ui.definition->page();
  connect(&inspectAction, &QAction::triggered, this, [page, this]()
          {
            if (inspectView == nullptr || !inspectView->isVisible()) {
                inspectView = new QWebEngineView();
                page->setDevToolsPage(inspectView->page());
                page->triggerAction(QWebEnginePage::InspectElement);
                inspectView->show();
            } 
          });

  ui.definition->installEventFilter( this );
  ui.searchFrame->installEventFilter( this );
  ui.ftsSearchFrame->installEventFilter( this );

  QWebEngineSettings * settings = ui.definition->page()->settings();
  settings->defaultSettings()->setAttribute( QWebEngineSettings::WebAttribute::LocalContentCanAccessRemoteUrls, true );
  settings->defaultSettings()->setAttribute( QWebEngineSettings::WebAttribute::LocalContentCanAccessFileUrls, true );
  settings->defaultSettings()->setAttribute( QWebEngineSettings::WebAttribute::ErrorPageEnabled, false);
  // Load the default blank page instantly, so there would be no flicker.

  QString contentType;
  QUrl blankPage( "gdlookup://localhost?blank=1" );

  sptr< Dictionary::DataRequest > r = articleNetMgr.getResource( blankPage,
                                                                 contentType );

  ui.definition->setHtml( QString::fromUtf8( &( r->getFullData().front() ),
                                             r->getFullData().size() ),
                          blankPage );

  expandOptionalParts = cfg.preferences.alwaysExpandOptionalParts;

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  ui.definition->grabGesture( Gestures::GDPinchGestureType );
  ui.definition->grabGesture( Gestures::GDSwipeGestureType );
#endif

  // Variable name for store current selection range
  rangeVarName = QString( "sr_%1" ).arg( QString::number( (quint64)this, 16 ) );

  connect(GlobalBroadcaster::instance(), SIGNAL( emitDictIds(ActiveDictIds)), this,
          SLOT(setActiveDictIds(ActiveDictIds)));

  channel = new QWebChannel(ui.definition->page());
  attachToJavaScript();
}

// explicitly report the minimum size, to avoid
// sidebar widgets' improper resize during restore
QSize ArticleView::minimumSizeHint() const
{
  return ui.searchFrame->minimumSizeHint();
}

void ArticleView::setGroupComboBox( GroupComboBox const * g )
{
  groupComboBox = g;
}

ArticleView::~ArticleView()
{
  cleanupTemp();
  audioPlayer->stop();
  channel->deregisterObject(this);
  ui.definition->ungrabGesture( Gestures::GDPinchGestureType );
  ui.definition->ungrabGesture( Gestures::GDSwipeGestureType );
}

void ArticleView::showDefinition( Config::InputPhrase const & phrase, unsigned group,
                                  QString const & scrollTo,
                                  Contexts const & contexts_ )
{
  currentWord = phrase.phrase;
  currentActiveDictIds.clear();
  // first, let's stop the player
  audioPlayer->stop();

  QUrl req;
  Contexts contexts( contexts_ );

  req.setScheme( "gdlookup" );
  req.setHost( "localhost" );
  Qt4x5::Url::addQueryItem( req, "word", phrase.phrase );
  if ( !phrase.punctuationSuffix.isEmpty() )
    Qt4x5::Url::addQueryItem( req, "punctuation_suffix", phrase.punctuationSuffix );
  Qt4x5::Url::addQueryItem( req, "group", QString::number( group ) );
  if( cfg.preferences.ignoreDiacritics )
    Qt4x5::Url::addQueryItem( req, "ignore_diacritics", "1" );

  if ( scrollTo.size() )
    Qt4x5::Url::addQueryItem( req, "scrollto", scrollTo );

  Contexts::Iterator pos = contexts.find( "gdanchor" );
  if( pos != contexts.end() )
  {
    Qt4x5::Url::addQueryItem( req, "gdanchor", contexts[ "gdanchor" ] );
    contexts.erase( pos );
  }

  if ( contexts.size() )
  {
    QBuffer buf;

    buf.open( QIODevice::WriteOnly );

    QDataStream stream( &buf );

    stream << contexts;

    buf.close();

    Qt4x5::Url::addQueryItem( req,  "contexts", QString::fromLatin1( buf.buffer().toBase64() ) );
  }

  QString mutedDicts = getMutedForGroup( group );

  if ( mutedDicts.size() )
    Qt4x5::Url::addQueryItem( req,  "muted", mutedDicts );

  // Update both histories (pages history and headwords history)
  saveHistoryUserData();
  emit sendWordToHistory( phrase.phrase );

  // Any search opened is probably irrelevant now
  closeSearch();

  // Clear highlight all button selection
  ui.highlightAllButton->setChecked(false);

  emit setExpandMode( expandOptionalParts );

  ui.definition->load( req );

  //QApplication::setOverrideCursor( Qt::WaitCursor );
  ui.definition->setCursor( Qt::WaitCursor );
}

void ArticleView::showDefinition( QString const & word, unsigned group,
                                  QString const & scrollTo,
                                  Contexts const & contexts_ )
{
  showDefinition( Config::InputPhrase::fromPhrase( word ), group, scrollTo, contexts_ );
}

void ArticleView::showDefinition( QString const & word, QStringList const & dictIDs,
                                  QRegExp const & searchRegExp, unsigned group,
                                  bool ignoreDiacritics )
{
  if( dictIDs.isEmpty() )
    return;

  // first, let's stop the player
  audioPlayer->stop();

  QUrl req;

  req.setScheme( "gdlookup" );
  req.setHost( "localhost" );
  Qt4x5::Url::addQueryItem( req, "word", word );
  Qt4x5::Url::addQueryItem( req, "dictionaries", dictIDs.join( ",") );
  Qt4x5::Url::addQueryItem( req, "regexp", searchRegExp.pattern() );
  if( searchRegExp.caseSensitivity() == Qt::CaseSensitive )
    Qt4x5::Url::addQueryItem( req, "matchcase", "1" );
  if( searchRegExp.patternSyntax() == QRegExp::WildcardUnix )
    Qt4x5::Url::addQueryItem( req, "wildcards", "1" );
  Qt4x5::Url::addQueryItem( req, "group", QString::number( group ) );
  if( ignoreDiacritics )
    Qt4x5::Url::addQueryItem( req, "ignore_diacritics", "1" );

  // Update both histories (pages history and headwords history)
  saveHistoryUserData();
  emit sendWordToHistory( word );

  // Any search opened is probably irrelevant now
  closeSearch();

  // Clear highlight all button selection
  ui.highlightAllButton->setChecked(false);

  emit setExpandMode( expandOptionalParts );

  ui.definition->load( req );

  //QApplication::setOverrideCursor( Qt::WaitCursor );
  ui.definition->setCursor( Qt::WaitCursor );
}

void ArticleView::showAnticipation()
{
  ui.definition->setHtml( "" );
  ui.definition->setCursor( Qt::WaitCursor );
  //QApplication::setOverrideCursor( Qt::WaitCursor );
}

void ArticleView::loadFinished( bool )
{
  setZoomFactor(cfg.preferences.zoomFactor);
  QUrl url = ui.definition->url();
  qDebug() << "article view loaded url:" << url.url ().left (200);

  QVariant userDataVariant = ui.definition->property("currentArticle");

  if ( userDataVariant.isValid() )
  {
    QString currentArticle = userDataVariant.toString();

    if ( !currentArticle.isEmpty() )
    {
      // There's an active article saved, so set it to be active.
      setCurrentArticle( currentArticle );
    }

    double sx = 0, sy = 0;

    QVariant qsx=ui.definition->property("sx");
    if ( qsx.type() == QVariant::Double )
      sx = qsx.toDouble();

    QVariant qsy = ui.definition->property("sx");
    if ( qsy.type() == QVariant::Double )
      sy = qsy.toDouble();

    if ( sx != 0 || sy != 0 )
    {
      // Restore scroll position
      ui.definition->page()->runJavaScript(
          QString( "window.scroll( %1, %2 );" ).arg( sx ).arg( sy ) );
    }
  }
  else
  {
    QString const scrollTo = Utils::Url::queryItemValue( url, "scrollto" );
    if( isScrollTo( scrollTo ) )
    {
      // There is no active article saved in history, but we have it as a parameter.
      // setCurrentArticle will save it and scroll there.
      setCurrentArticle( scrollTo, true );
    }
  }


  ui.definition->unsetCursor();
  //QApplication::restoreOverrideCursor();

  // Expand collapsed article if only one loaded
  ui.definition->page()->runJavaScript( QString( "gdCheckArticlesNumber();" ) );

  // Jump to current article after page reloading
  if( !articleToJump.isEmpty() )
  {
    setCurrentArticle( articleToJump, true );
    articleToJump.clear();
  }

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  if( !Qt4x5::Url::queryItemValue( url, "gdanchor" ).isEmpty() )
  {
    QString anchor = QUrl::fromPercentEncoding( Qt4x5::Url::encodedQueryItemValue( url, "gdanchor" ) );

    // Find GD anchor on page

    int n = anchor.indexOf( '_' );
    if( n == 33 )
      // MDict pattern: ("g" + dictionary ID (33 chars total))_(articleID(quint64, hex))_(original anchor)
      n = anchor.indexOf( '_', n + 1 );
    else
      n = 0;

    if( n > 0 )
    {
      QString originalAnchor = anchor.mid( n + 1 );

      int end = originalAnchor.indexOf('_');
      QString hash=originalAnchor.left(end);
      url.clear();
      url.setFragment(hash);
      ui.definition->page()->runJavaScript(
          QString("window.location.hash = \"%1\"").arg(QString::fromUtf8(url.toEncoded())));
      
    }
    else
    {
      url.clear();
      url.setFragment( anchor );
      ui.definition->page()->runJavaScript(
         QString( "window.location.hash = \"%1\"" ).arg( QString::fromUtf8( url.toEncoded() ) ) );
    }
  }
#endif

  emit pageLoaded( this );

  if( Qt4x5::Url::hasQueryItem( ui.definition->url(), "regexp" ) )
    highlightFTSResults();
}

void ArticleView::handleTitleChanged( QString const & title )
{
  if( !title.isEmpty() ) // Qt 5.x WebKit raise signal titleChanges(QString()) while navigation within page
    emit titleChanged( this, title );
}

void ArticleView::handleUrlChanged( QUrl const & url )
{
  lastUrl = url.url();
  QIcon icon;

  unsigned group = getGroup( url );

  if ( group )
  {
    // Find the group's instance corresponding to the fragment value
    for( unsigned x = 0; x < groups.size(); ++x )
      if ( groups[ x ].id == group )
      {
        // Found it

        icon = groups[ x ].makeIcon();
        break;
      }
  }

  emit iconChanged( this, icon );
}

unsigned ArticleView::getGroup( QUrl const & url )
{
  if ( url.scheme() == "gdlookup" && Qt4x5::Url::hasQueryItem( url, "group" ) )
    return Qt4x5::Url::queryItemValue( url, "group" ).toUInt();

  return 0;
}

QStringList ArticleView::getArticlesList()
{
  return runJavaScriptSync( ui.definition->page(), "gdArticleContents" )
    .trimmed().split( ' ', QString::SkipEmptyParts );
}

QString ArticleView::getActiveArticleId()
{
  QString currentArticle = getCurrentArticle();
  if ( !isScrollTo( currentArticle ) )
    return QString(); // Incorrect id

  return dictionaryIdFromScrollTo( currentArticle );
}

QString ArticleView::getCurrentArticle()
{
  QVariant v=ui.definition->property("currentArticle");

  if ( v.type() == QVariant::String )
    return v.toString();
  else
    return QString();
}

void ArticleView::jumpToDictionary( QString const & id, bool force )
{
  QString targetArticle = scrollToFromDictionaryId( id );

  // jump only if neceessary, or when forced
  if ( force || targetArticle != getCurrentArticle() )
  {
    setCurrentArticle( targetArticle, true );
  }
}

void ArticleView::setCurrentArticle( QString const & id, bool moveToIt )
{
  if ( !isScrollTo( id ) )
    return; // Incorrect id

  if ( !ui.definition->isVisible() )
    return; // No action on background page, scrollIntoView there don't work

  if(moveToIt){
      QString script=QString(" var elem=document.getElementById('%1'); if(elem!=undefined){elem.scrollIntoView(true);}").arg(id);
      ui.definition->page()->runJavaScript(script);
      ui.definition->setProperty("currentArticle",id);
  }
}

void ArticleView::selectCurrentArticle()
{
  ui.definition->page()->runJavaScript(
        QString( "gdSelectArticle( '%1' );" ).arg( getActiveArticleId() ) );
}

bool ArticleView::isFramedArticle( QString const & ca )
{
  if ( ca.isEmpty() )
    return false;

  QString result=runJavaScriptSync( ui.definition->page(), QString( "!!document.getElementById('gdexpandframe-%1');" ).arg( ca.mid( 7 ) ) );
  return result=="true";
}

bool ArticleView::isExternalLink( QUrl const & url )
{
  return url.scheme() == "http" || url.scheme() == "https" ||
         url.scheme() == "ftp" || url.scheme() == "mailto" ||
         url.scheme() == "file";
}

void ArticleView::tryMangleWebsiteClickedUrl( QUrl & url, Contexts & contexts )
{
  // Don't try mangling audio urls, even if they are from the framed websites

  if( ( url.scheme() == "http" || url.scheme() == "https" )
      && ! Dictionary::WebMultimediaDownload::isAudioUrl( url ) )
  {
    // Maybe a link inside a website was clicked?

    QString ca = getCurrentArticle();

    if ( isFramedArticle( ca ) )
    {
    	//todo ,ÓÃ±äÁ¿´úÌæ×îºóµÄurl
      //QVariant result = runJavaScriptSync( ui.definition->page(), "gdLastUrlText" );
      QVariant result ;

      if ( result.type() == QVariant::String )
      {
        // Looks this way
        contexts[ dictionaryIdFromScrollTo( ca ) ] = QString::fromLatin1( url.toEncoded() );

        QUrl target;

        QString queryWord = result.toString();

        // Empty requests are treated as no request, so we work this around by
        // adding a space.
        if ( queryWord.isEmpty() )
          queryWord = " ";

        target.setScheme( "gdlookup" );
        target.setHost( "localhost" );
        target.setPath( "/" + queryWord );

        url = target;
      }
    }
  }
}

void ArticleView::updateCurrentArticleFromCurrentFrame( QWebEnginePage * frame ,QPoint * point)
{

}

void ArticleView::saveHistoryUserData()
{
  ui.definition->setProperty("currentArticle", getCurrentArticle());
  ui.definition->setProperty("sx", ui.definition->page()->scrollPosition().x());
  ui.definition->setProperty("sy", ui.definition->page()->scrollPosition().y());
}

void ArticleView::cleanupTemp()
{
  QSet< QString >::iterator it = desktopOpenedTempFiles.begin();
  while( it != desktopOpenedTempFiles.end() )
  {
    if( QFile::remove( *it ) )
      it = desktopOpenedTempFiles.erase( it );
    else
      ++it;
  }
}

bool ArticleView::handleF3( QObject * /*obj*/, QEvent * ev )
{
  if ( ev->type() == QEvent::ShortcutOverride
       || ev->type() == QEvent::KeyPress )
  {
    QKeyEvent * ke = static_cast<QKeyEvent *>( ev );
    if ( ke->key() == Qt::Key_F3 && isSearchOpened() ) {
      if ( !ke->modifiers() )
      {
        if( ev->type() == QEvent::KeyPress )
          on_searchNext_clicked();

        ev->accept();
        return true;
      }

      if ( ke->modifiers() == Qt::ShiftModifier )
      {
        if( ev->type() == QEvent::KeyPress )
          on_searchPrevious_clicked();

        ev->accept();
        return true;
      }
    }
    if ( ke->key() == Qt::Key_F3 && ftsSearchIsOpened )
    {
      if ( !ke->modifiers() )
      {
        if( ev->type() == QEvent::KeyPress )
          on_ftsSearchNext_clicked();

        ev->accept();
        return true;
      }

      if ( ke->modifiers() == Qt::ShiftModifier )
      {
        if( ev->type() == QEvent::KeyPress )
          on_ftsSearchPrevious_clicked();

        ev->accept();
        return true;
      }
    }
  }

  return false;
}

bool ArticleView::eventFilter( QObject * obj, QEvent * ev )
{
#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  if( ev->type() == QEvent::Gesture )
  {
    Gestures::GestureResult result;
    QPoint pt;

    bool handled = Gestures::handleGestureEvent( obj, ev, result, pt );

    if( handled )
    {
      if( result == Gestures::ZOOM_IN )
        zoomIn();
      else
      if( result == Gestures::ZOOM_OUT )
        zoomOut();
      else
      if( result == Gestures::SWIPE_LEFT )
        back();
      else
      if( result == Gestures::SWIPE_RIGHT )
        forward();
      else
      if( result == Gestures::SWIPE_UP || result == Gestures::SWIPE_DOWN )
      {
        int delta = result == Gestures::SWIPE_UP ? -120 : 120;
        QWidget *widget = static_cast< QWidget * >( obj );

        QWidget *child = widget->childAt( widget->mapFromGlobal( pt ) );
        if( child )
        {
          QWheelEvent whev( child->mapFromGlobal( pt ), pt, delta, Qt::NoButton, Qt::NoModifier );
          qApp->sendEvent( child, &whev );
        }
        else
        {
          QWheelEvent whev( widget->mapFromGlobal( pt ), pt, delta, Qt::NoButton, Qt::NoModifier );
          qApp->sendEvent( widget, &whev );
        }
      }
    }

    return handled;
  }

  if( ev->type() == QEvent::MouseMove )
  {
    if( Gestures::isFewTouchPointsPresented() )
    {
      ev->accept();
      return true;
    }
  }
#endif

  if ( handleF3( obj, ev ) )
  {
    return true;
  }

  if ( obj == ui.definition )
  {
    if ( ev->type() == QEvent::MouseButtonPress ) {
      QMouseEvent * event = static_cast< QMouseEvent * >( ev );
      if ( event->button() == Qt::XButton1 ) {
        back();
        return true;
      }
      if ( event->button() == Qt::XButton2 ) {
        forward();
        return true;
      }
    }
    else
    if ( ev->type() == QEvent::KeyPress || ev->type ()==QEvent::ShortcutOverride)
    {
      QKeyEvent * keyEvent = static_cast< QKeyEvent * >( ev );

      if ( keyEvent->modifiers() &
           ( Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier ) )
        return false; // A non-typing modifier is pressed

      if ( keyEvent->key() == Qt::Key_Space ||
           keyEvent->key() == Qt::Key_Backspace ||
           keyEvent->key() == Qt::Key_Tab ||
           keyEvent->key() == Qt::Key_Backtab ||
           keyEvent->key() == Qt::Key_Return ||
           keyEvent->key() == Qt::Key_Enter ||
           keyEvent->key() == Qt::Key_Escape)
        return false; // Those key have other uses than to start typing

      QString text = keyEvent->text();

      if ( text.size() )
      {
        emit typingEvent( text );
        return true;
      }
    }
  }
  else
    return QFrame::eventFilter( obj, ev );

  return false;
}

QString ArticleView::getMutedForGroup( unsigned group )
{
  if ( dictionaryBarToggled && dictionaryBarToggled->isChecked() )
  {
    // Dictionary bar is active -- mute the muted dictionaries
    Instances::Group const * groupInstance = groups.findGroup( group );

    // Find muted dictionaries for current group
    Config::Group const * grp = cfg.getGroup( group );
    Config::MutedDictionaries const * mutedDictionaries;
    if( group == Instances::Group::AllGroupId )
      mutedDictionaries = popupView ? &cfg.popupMutedDictionaries : &cfg.mutedDictionaries;
    else
        mutedDictionaries = grp ? ( popupView ? &grp->popupMutedDictionaries : &grp->mutedDictionaries ) : 0;
    if( !mutedDictionaries )
      return QString();

    QStringList mutedDicts;

    if ( groupInstance )
    {
      for( unsigned x = 0; x < groupInstance->dictionaries.size(); ++x )
      {
        QString id = QString::fromStdString(
                       groupInstance->dictionaries[ x ]->getId() );

        if ( mutedDictionaries->contains( id ) )
          mutedDicts.append( id );
      }
    }

    if ( mutedDicts.size() )
      return mutedDicts.join( "," );
  }

  return QString();
}

QStringList ArticleView::getMutedDictionaries(unsigned group) {
  if (dictionaryBarToggled && dictionaryBarToggled->isChecked()) {
    // Dictionary bar is active -- mute the muted dictionaries
    Instances::Group const *groupInstance = groups.findGroup(group);

		// Find muted dictionaries for current group
		Config::Group const* grp = cfg.getGroup(group);
		Config::MutedDictionaries const* mutedDictionaries;
        if (group == Instances::Group::AllGroupId)
			mutedDictionaries = popupView ? &cfg.popupMutedDictionaries : &cfg.mutedDictionaries;
		else
			mutedDictionaries = grp ? (popupView ? &grp->popupMutedDictionaries : &grp->mutedDictionaries) : 0;
		if (!mutedDictionaries)
			return QStringList();

    QStringList mutedDicts;

    if (groupInstance) {
      for (unsigned x = 0; x < groupInstance->dictionaries.size(); ++x) {
        QString id = QString::fromStdString(groupInstance->dictionaries[x]->getId());

        if (mutedDictionaries->contains(id))
          mutedDicts.append(id);
      }
    }

    return mutedDicts;
  }

  return QStringList();
}

void ArticleView::linkHovered ( const QString & link )
{
  QString msg;
  QUrl url(link);

  if ( url.scheme() == "bres" )
  {
    msg = tr( "Resource" );
  }
  else
  if ( url.scheme() == "gdau" || Dictionary::WebMultimediaDownload::isAudioUrl( url ) )
  {
    msg = tr( "Audio" );
  }
  else
  if ( url.scheme() == "gdtts" )
  {
    msg = tr( "TTS Voice" );
  }
  else
  if ( url.scheme() == "gdpicture" )
  {
    msg = tr( "Picture" );
  }
  else
  if ( url.scheme() == "gdvideo" )
  {
    if ( url.path().isEmpty() )
    {
      msg = tr( "Video" );
    }
    else
    {
      QString path = url.path();
      if ( path.startsWith( '/' ) )
      {
        path = path.mid( 1 );
      }
      msg = tr( "Video: %1" ).arg( path );
    }
  }
  else
  if (url.scheme() == "gdlookup" || url.scheme().compare( "bword" ) == 0)
  {
    QString def = url.path();
    if (def.startsWith("/"))
    {
      def = def.mid( 1 );
    }

    if( Qt4x5::Url::hasQueryItem( url, "dict" ) )
    {
      // Link to other dictionary
      QString dictName( Qt4x5::Url::queryItemValue( url, "dict" ) );
      if( !dictName.isEmpty() )
        msg = tr( "Definition from dictionary \"%1\": %2" ).arg( dictName ).arg( def );
    }

    if( msg.isEmpty() )
    {
      if( def.isEmpty() && url.hasFragment() )
        msg = '#' + url.fragment(); // this must be a citation, footnote or backlink
      else
        msg = tr( "Definition: %1").arg( def );
    }
  }
  else
  {
    msg = link;
  }

  emit statusBarMessage( msg );
}

void ArticleView::attachToJavaScript() {


  // set the web channel to be used by the page
  // see http://doc.qt.io/qt-5/qwebenginepage.html#setWebChannel
  ui.definition->page()->setWebChannel(channel, QWebEngineScript::MainWorld);

  // register QObjects to be exposed to JavaScript
  channel->registerObject(QStringLiteral("articleview"), this);
}

void ArticleView::linkClicked( QUrl const & url_ )
{
  Qt::KeyboardModifiers kmod = QApplication::keyboardModifiers();

  // Lock jump on links while Alt key is pressed
  if( kmod & Qt::AltModifier )
    return;

  updateCurrentArticleFromCurrentFrame();

  QUrl url( url_ );
  Contexts contexts;

  tryMangleWebsiteClickedUrl( url, contexts );

  if ( !popupView &&
       ( ui.definition->isMidButtonPressed() ||
         ( kmod & ( Qt::ControlModifier | Qt::ShiftModifier ) ) ) )
  {
    // Mid button or Control/Shift is currently pressed - open the link in new tab
    emit openLinkInNewTab( url, ui.definition->url(), getCurrentArticle(), contexts );
  }
  else
    openLink( url, ui.definition->url(), getCurrentArticle(), contexts );
}

void ArticleView::linkClickedInHtml( QUrl const & url_ )
{
  ui.definition->linkClickedInHtml(url_);
}
void ArticleView::openLink( QUrl const & url, QUrl const & ref,
                            QString const & scrollTo,
                            Contexts const & contexts_ )
{
  qDebug() << "open link url:" << url;

  Contexts contexts( contexts_ );

  if( url.scheme().compare( "gdpicture" ) == 0 )
    ui.definition->load( url );
  else
  if ( url.scheme().compare( "bword" ) == 0 )
  {
    if( Qt4x5::Url::hasQueryItem( ref, "dictionaries" ) )
    {
      QStringList dictsList = Qt4x5::Url::queryItemValue( ref, "dictionaries" )
                                          .split( ",", QString::SkipEmptyParts );

      showDefinition( url.path(), dictsList, QRegExp(), getGroup( ref ), false );
    }
    else
      showDefinition( url.path(),
                      getGroup( ref ), scrollTo, contexts );
  }
  else
  if ( url.scheme() == "gdlookup" ) // Plain html links inherit gdlookup scheme
  {
    if ( url.hasFragment() )
    {
      ui.definition->page()->runJavaScript(
        QString( "window.location = \"%1\"" ).arg( QString::fromUtf8( url.toEncoded() ) ) );
    }
    else
    {
      if( Qt4x5::Url::hasQueryItem( ref, "dictionaries" ) )
      {
        // Specific dictionary group from full-text search
        QStringList dictsList = Qt4x5::Url::queryItemValue( ref, "dictionaries" )
                                            .split( ",", QString::SkipEmptyParts );

        showDefinition( url.path().mid( 1 ), dictsList, QRegExp(), getGroup( ref ), false );
        return;
      }

      QString word;

      if( Utils::Url::hasQueryItem( url, "word" ) )
      {
          word=Utils::Url::queryItemValue (url,"word");
      }
      else{
          word=url.path ().mid (1);
      }

      QString newScrollTo( scrollTo );
      if( Qt4x5::Url::hasQueryItem( url, "dict" ) )
      {
        // Link to other dictionary
        QString dictName( Qt4x5::Url::queryItemValue( url, "dict" ) );
        for( unsigned i = 0; i < allDictionaries.size(); i++ )
        {
          if( dictName.compare( QString::fromUtf8( allDictionaries[ i ]->getName().c_str() ) ) == 0 )
          {
            newScrollTo = scrollToFromDictionaryId( QString::fromUtf8( allDictionaries[ i ]->getId().c_str() ) );
            break;
          }
        }
      }

      if( Qt4x5::Url::hasQueryItem( url, "gdanchor" ) )
        contexts[ "gdanchor" ] = Qt4x5::Url::queryItemValue( url, "gdanchor" );

      showDefinition( word,
                      getGroup( ref ), newScrollTo, contexts );
    }
  }
  else
  if ( url.scheme() == "bres" || url.scheme() == "gdau" || url.scheme() == "gdvideo" ||
       Dictionary::WebMultimediaDownload::isAudioUrl( url ) )
  {
    // Download it

    // Clear any pending ones

    resourceDownloadRequests.clear();

    resourceDownloadUrl = url;

    if ( Dictionary::WebMultimediaDownload::isAudioUrl( url ) )
    {
      sptr< Dictionary::DataRequest > req =
        new Dictionary::WebMultimediaDownload( url, articleNetMgr );

      resourceDownloadRequests.push_back( req );

      connect( req.get(), SIGNAL( finished() ),
               this, SLOT( resourceDownloadFinished() ) );
    }
    else
    if ( url.scheme() == "gdau" && url.host() == "search" )
    {
      // Since searches should be limited to current group, we just do them
      // here ourselves since otherwise we'd need to pass group id to netmgr
      // and it should've been having knowledge of the current groups, too.

      unsigned currentGroup = getGroup( ref );

      std::vector< sptr< Dictionary::Class > > const * activeDicts = 0;

      if ( groups.size() )
      {
        for( unsigned x = 0; x < groups.size(); ++x )
          if ( groups[ x ].id == currentGroup )
          {
            activeDicts = &( groups[ x ].dictionaries );
            break;
          }
      }
      else
        activeDicts = &allDictionaries;

      if ( activeDicts )
      {
        unsigned preferred = UINT_MAX;
        if( url.hasFragment() )
        {
          // Find sound in the preferred dictionary
          QString preferredName = Qt4x5::Url::fragment( url );
          try
          {
            for( unsigned x = 0; x < activeDicts->size(); ++x )
            {
              if( preferredName.compare( QString::fromUtf8( (*activeDicts)[ x ]->getName().c_str() ) ) == 0 )
              {
                preferred = x;
                sptr< Dictionary::DataRequest > req =
                  (*activeDicts)[ x ]->getResource(
                    url.path().mid( 1 ).toUtf8().data() );

                resourceDownloadRequests.push_back( req );

                if ( !req->isFinished() )
                {
                  // Queued loading
                  connect( req.get(), SIGNAL( finished() ),
                           this, SLOT( resourceDownloadFinished() ) );
                }
                else
                {
                  // Immediate loading
                  if( req->dataSize() > 0 )
                  {
                    // Resource already found, stop next search
                    resourceDownloadFinished();
                    return;
                  }
                }
                break;
              }
            }
          }
          catch( std::exception & e )
          {
            emit statusBarMessage(
                  tr( "ERROR: %1" ).arg( e.what() ),
                  10000, QPixmap( ":/icons/error.png" ) );
          }
        }
        for( unsigned x = 0; x < activeDicts->size(); ++x )
        {
          try
          {
            if( x == preferred )
              continue;

            sptr< Dictionary::DataRequest > req =
              (*activeDicts)[ x ]->getResource(
                url.path().mid( 1 ).toUtf8().data() );

            resourceDownloadRequests.push_back( req );

            if ( !req->isFinished() )
            {
              // Queued loading
              connect( req.get(), SIGNAL( finished() ),
                       this, SLOT( resourceDownloadFinished() ) );
            }
            else
            {
              // Immediate loading
              if( req->dataSize() > 0 )
              {
                // Resource already found, stop next search
                break;
              }
            }
          }
          catch( std::exception & e )
          {
            emit statusBarMessage(
                  tr( "ERROR: %1" ).arg( e.what() ),
                  10000, QPixmap( ":/icons/error.png" ) );
          }
        }
      }
    }
    else
    {
      // Normal resource download
      QString contentType;

      sptr< Dictionary::DataRequest > req =
        articleNetMgr.getResource( url, contentType );

      if ( !req.get() )
      {
        // Request failed, fail
      }
      else
      if ( req->isFinished() && req->dataSize() >= 0 )
      {
        // Have data ready, handle it
        resourceDownloadRequests.push_back( req );
        resourceDownloadFinished();

        return;
      }
      else
      if ( !req->isFinished() )
      {
        // Queue to be handled when done

        resourceDownloadRequests.push_back( req );

        connect( req.get(), SIGNAL( finished() ),
                 this, SLOT( resourceDownloadFinished() ) );
      }
    }

    if ( resourceDownloadRequests.empty() ) // No requests were queued
    {
      QMessageBox::critical( this, "GoldenDict", tr( "The referenced resource doesn't exist." ) );
      return;
    }
    else
      resourceDownloadFinished(); // Check any requests finished already
  }
  else
  if ( url.scheme() == "gdprg" )
  {
    // Program. Run it.
    QString id( url.host() );

    for( Config::Programs::const_iterator i = cfg.programs.begin();
         i != cfg.programs.end(); ++i )
    {
      if ( i->id == id )
      {
        // Found the corresponding program.
        Programs::RunInstance * req = new Programs::RunInstance;

        connect( req, SIGNAL(finished(QByteArray,QString)),
                 req, SLOT( deleteLater() ) );

        QString error;

        // Delete the request if it fails to start
        if ( !req->start( *i, url.path().mid( 1 ), error ) )
        {
          delete req;

          QMessageBox::critical( this, "GoldenDict",
                                 error );
        }

        return;
      }
    }

    // Still here? No such program exists.
    QMessageBox::critical( this, "GoldenDict",
                           tr( "The referenced audio program doesn't exist." ) );
  }
  else
  if ( url.scheme() == "gdtts" )
  {
// TODO: Port TTS
#if defined( Q_OS_WIN32 ) || defined( Q_OS_MAC )
    // Text to speech
    QString md5Id = Qt4x5::Url::queryItemValue( url, "engine" );
    QString text( url.path().mid( 1 ) );

    for ( Config::VoiceEngines::const_iterator i = cfg.voiceEngines.begin();
          i != cfg.voiceEngines.end(); ++i )
    {
      QString itemMd5Id = QString( QCryptographicHash::hash(
                                     i->id.toUtf8(),
                                     QCryptographicHash::Md5 ).toHex() );

      if ( itemMd5Id == md5Id )
      {
        SpeechClient * speechClient = new SpeechClient( *i, this );
        connect( speechClient, SIGNAL( finished() ), speechClient, SLOT( deleteLater() ) );
        speechClient->tell( text );
        break;
      }
    }
#endif
  }
  else
  if ( isExternalLink( url ) )
  {
    // Use the system handler for the conventional external links
    QDesktopServices::openUrl( url );
  }
}

ResourceToSaveHandler * ArticleView::saveResource( const QUrl & url, const QString & fileName )
{
  return saveResource( url, ui.definition->url(), fileName );
}

ResourceToSaveHandler * ArticleView::saveResource( const QUrl & url, const QUrl & ref, const QString & fileName )
{
  ResourceToSaveHandler * handler = new ResourceToSaveHandler( this, fileName );
  sptr< Dictionary::DataRequest > req;

  if( url.scheme() == "bres" || url.scheme() == "gico" || url.scheme() == "gdau" || url.scheme() == "gdvideo" )
  {
    if ( url.host() == "search" )
    {
      // Since searches should be limited to current group, we just do them
      // here ourselves since otherwise we'd need to pass group id to netmgr
      // and it should've been having knowledge of the current groups, too.

      unsigned currentGroup = getGroup( ref );

      std::vector< sptr< Dictionary::Class > > const * activeDicts = 0;

      if ( groups.size() )
      {
        for( unsigned x = 0; x < groups.size(); ++x )
          if ( groups[ x ].id == currentGroup )
          {
            activeDicts = &( groups[ x ].dictionaries );
            break;
          }
      }
      else
        activeDicts = &allDictionaries;

      if ( activeDicts )
      {
        unsigned preferred = UINT_MAX;
        if( url.hasFragment() && url.scheme() == "gdau" )
        {
          // Find sound in the preferred dictionary
          QString preferredName = Qt4x5::Url::fragment( url );
          for( unsigned x = 0; x < activeDicts->size(); ++x )
          {
            try
            {
              if( preferredName.compare( QString::fromUtf8( (*activeDicts)[ x ]->getName().c_str() ) ) == 0 )
              {
                preferred = x;
                sptr< Dictionary::DataRequest > req =
                  (*activeDicts)[ x ]->getResource(
                    url.path().mid( 1 ).toUtf8().data() );

                handler->addRequest( req );

                if( req->isFinished() && req->dataSize() > 0 )
                {
                  handler->downloadFinished();
                  return handler;
                }
                break;
              }
            }
            catch( std::exception & e )
            {
              gdWarning( "getResource request error (%s) in \"%s\"\n", e.what(),
                         (*activeDicts)[ x ]->getName().c_str() );
            }
          }
        }
        for( unsigned x = 0; x < activeDicts->size(); ++x )
        {
          try
          {
            if( x == preferred )
              continue;

            req = (*activeDicts)[ x ]->getResource(
                    Qt4x5::Url::path( url ).mid( 1 ).toUtf8().data() );

            handler->addRequest( req );

            if( req->isFinished() && req->dataSize() > 0 )
            {
              // Resource already found, stop next search
              break;
            }
          }
          catch( std::exception & e )
          {
            gdWarning( "getResource request error (%s) in \"%s\"\n", e.what(),
                       (*activeDicts)[ x ]->getName().c_str() );
          }
        }
      }
    }
    else
    {
      // Normal resource download
      QString contentType;
      req = articleNetMgr.getResource( url, contentType );

      if( req.get() )
      {
        handler->addRequest( req );
      }
    }
  }
  else
  {
    req = new Dictionary::WebMultimediaDownload( url, articleNetMgr );

    handler->addRequest( req );
  }

  if ( handler->isEmpty() ) // No requests were queued
  {
    emit statusBarMessage(
          tr( "ERROR: %1" ).arg( tr( "The referenced resource doesn't exist." ) ),
          10000, QPixmap( ":/icons/error.png" ) );
  }

  // Check already finished downloads
  handler->downloadFinished();

  return handler;
}

void ArticleView::updateMutedContents()
{
  QUrl currentUrl = ui.definition->url();

  if ( currentUrl.scheme() != "gdlookup" )
    return; // Weird url -- do nothing

  unsigned group = getGroup( currentUrl );

  if ( !group )
    return; // No group in url -- do nothing

  QString mutedDicts = getMutedForGroup( group );

  if ( Qt4x5::Url::queryItemValue( currentUrl, "muted" ) != mutedDicts )
  {
    // The list has changed -- update the url

    Qt4x5::Url::removeQueryItem( currentUrl, "muted" );

    if ( mutedDicts.size() )
    Qt4x5::Url::addQueryItem( currentUrl, "muted", mutedDicts );

    saveHistoryUserData();

    ui.definition->load( currentUrl );

    //QApplication::setOverrideCursor( Qt::WaitCursor );
    ui.definition->setCursor( Qt::WaitCursor );
  }
}

bool ArticleView::canGoBack()
{
  // First entry in a history is always an empty page,
  // so we skip it.
  return ui.definition->history()->currentItemIndex() > 1;
}

bool ArticleView::canGoForward()
{
  return ui.definition->history()->canGoForward();
}

void ArticleView::setSelectionBySingleClick( bool set )
{
  ui.definition->setSelectionBySingleClick( set );
}

void ArticleView::back()
{
  // Don't allow navigating back to page 0, which is usually the initial
  // empty page
  if ( canGoBack() )
  {
    saveHistoryUserData();
    ui.definition->back();
  }
}

void ArticleView::forward()
{
  saveHistoryUserData();
  ui.definition->forward();
}

bool ArticleView::hasSound()
{
  QVariant v = runJavaScriptSync( ui.definition->page(),"gdAudioLinks.first" );
  if ( v.type() == QVariant::String )
    return !v.toString().isEmpty();
  return false;
}

//use webengine javascript to playsound
void ArticleView::playSound()
{
  QString variable = "   var link=gdAudioLinks[gdAudioLinks.current];           "
    "   if(link==undefined){           "
    "       link=gdAudioLinks.first;           "
    "   }          "
    "              "
    "   var music = new Audio(link);    "
    "   music.play();   ";
  ui.definition->page()->runJavaScript(variable);
}

// use eventloop to turn the async callback to sync execution.
QString ArticleView::toHtml()
{
    QString result;
    QSharedPointer<QEventLoop> loop = QSharedPointer<QEventLoop>(new QEventLoop());
    QTimer::singleShot(1000, loop.data(), &QEventLoop::quit);

    ui.definition->page()->toHtml([loop, &result](const QString &content) {
        if (loop->isRunning()) {
            result = content;
            loop->quit();
        }
    });
    loop->exec();
    return result;
}

void ArticleView::setHtml(const QString& content,const QUrl& baseUrl){
    ui.definition->page()->setHtml(content,baseUrl);
}

void ArticleView::setContent(const QByteArray &data, const QString &mimeType, const QUrl &baseUrl ){
    ui.definition->page()->setContent(data,mimeType,baseUrl);
}

QString ArticleView::getTitle()
{
  return ui.definition->page()->title();
}

Config::InputPhrase ArticleView::getPhrase() const
{
  const QUrl url = ui.definition->url();
  return Config::InputPhrase( Qt4x5::Url::queryItemValue( url, "word" ),
                              Qt4x5::Url::queryItemValue( url, "punctuation_suffix" ) );
}

void ArticleView::print( QPrinter * printer ) const
{
    ui.definition->page()->print(printer, [](bool result) {});
}

void ArticleView::contextMenuRequested( QPoint const & pos )
{
  // Is that a link? Is there a selection?

  QWebEnginePage* r=ui.definition->page();
  updateCurrentArticleFromCurrentFrame(ui.definition->page(), const_cast<QPoint *>(& pos));

  QMenu menu( this );

  QAction * followLink = 0;
  QAction * followLinkExternal = 0;
  QAction * followLinkNewTab = 0;
  QAction * lookupSelection = 0;
  QAction * lookupSelectionGr = 0;
  QAction * lookupSelectionNewTab = 0;
  QAction * lookupSelectionNewTabGr = 0;
  QAction * maxDictionaryRefsAction = 0;
  QAction * addWordToHistoryAction = 0;
  QAction * addHeaderToHistoryAction = 0;
  QAction * sendWordToInputLineAction = 0;
  QAction * saveImageAction = 0;
  QAction * saveSoundAction = 0;

  QWebEngineContextMenuData menuData=r->contextMenuData();
  QUrl targetUrl(menuData.linkUrl());
  qDebug() << "menu:" <<  menuData.linkText()<<":"<<menuData.mediaUrl();
  Contexts contexts;

  tryMangleWebsiteClickedUrl( targetUrl, contexts );

  if ( !targetUrl.isEmpty() )
  {
    if ( !isExternalLink( targetUrl ) )
    {
      followLink = new QAction( tr( "&Open Link" ), &menu );
      menu.addAction( followLink );

      if ( !popupView )
      {
        followLinkNewTab = new QAction( QIcon( ":/icons/addtab.png" ),
                                        tr( "Open Link in New &Tab" ), &menu );
        menu.addAction( followLinkNewTab );
      }
    }

    if ( isExternalLink( targetUrl ) )
    {
      followLinkExternal = new QAction( tr( "Open Link in &External Browser" ), &menu );
      menu.addAction( followLinkExternal );
      menu.addAction( ui.definition->pageAction( QWebEnginePage::CopyLinkToClipboard ) );
    }
  }

  QUrl imageUrl;
  if( !popupView && menuData.mediaType ()==QWebEngineContextMenuData::MediaTypeImage)
  {
    imageUrl = menuData.mediaUrl();
    if (!imageUrl.isEmpty())
    {
      menu.addAction(ui.definition->pageAction(QWebEnginePage::CopyImageToClipboard));
      saveImageAction = new QAction(tr("Save &image..."), &menu);
      menu.addAction(saveImageAction);
    }
  }

  if (!popupView && (targetUrl.scheme() == "gdau" || Dictionary::WebMultimediaDownload::isAudioUrl(targetUrl)))
  {
    saveSoundAction = new QAction(tr("Save s&ound..."), &menu);
    menu.addAction(saveSoundAction);
  }

  QString selectedText = ui.definition->selectedText();
  QString text = selectedText.trimmed();

  if ( text.size() && text.size() < 60 )
  {
    // We don't prompt for selections larger or equal to 60 chars, since
    // it ruins the menu and it's hardly a single word anyway.

    if( text.isRightToLeft() )
    {
      text.insert( 0, (ushort)0x202E ); // RLE, Right-to-Left Embedding
      text.append( (ushort)0x202C ); // PDF, POP DIRECTIONAL FORMATTING
    }

    lookupSelection = new QAction( tr( "&Look up \"%1\"" ).
                                   arg( text ),
                                   &menu );
    menu.addAction( lookupSelection );

    if ( !popupView )
    {
      lookupSelectionNewTab = new QAction( QIcon( ":/icons/addtab.png" ),
                                           tr( "Look up \"%1\" in &New Tab" ).
                                           arg( text ),
                                           &menu );
      menu.addAction( lookupSelectionNewTab );

      sendWordToInputLineAction = new QAction( tr( "Send \"%1\" to input line" ).
                                               arg( text ),
                                               &menu );
      menu.addAction( sendWordToInputLineAction );
    }

    addWordToHistoryAction = new QAction( tr( "&Add \"%1\" to history" ).
                                          arg( text ),
                                          &menu );
    menu.addAction( addWordToHistoryAction );

    Instances::Group const * altGroup =
      ( groupComboBox && groupComboBox->getCurrentGroup() !=  getGroup( ui.definition->url() )  ) ?
        groups.findGroup( groupComboBox->getCurrentGroup() ) : 0;

    if ( altGroup )
    {
      QIcon icon = altGroup->icon.size() ? QIcon( ":/flags/" + altGroup->icon ) :
                   QIcon();

      lookupSelectionGr = new QAction( icon, tr( "Look up \"%1\" in %2" ).
                                       arg( text ).
                                       arg( altGroup->name ), &menu );
      menu.addAction( lookupSelectionGr );

      if ( !popupView )
      {
        lookupSelectionNewTabGr = new QAction( QIcon( ":/icons/addtab.png" ),
                                               tr( "Look up \"%1\" in %2 in &New Tab" ).
                                               arg( text ).
                                               arg( altGroup->name ), &menu );
        menu.addAction( lookupSelectionNewTabGr );
      }
    }
  }

  if( text.isEmpty() && !cfg.preferences.storeHistory)
  {
    QString txt = ui.definition->title();
    if( txt.size() > 60 )
      txt = txt.left( 60 ) + "...";

    addHeaderToHistoryAction = new QAction( tr( "&Add \"%1\" to history" ).
                                            arg( txt ),
                                            &menu );
    menu.addAction( addHeaderToHistoryAction );
  }

  if ( selectedText.size() )
  {
    menu.addAction( ui.definition->pageAction( QWebEnginePage::Copy ) );
    menu.addAction( &copyAsTextAction );
  }
  else
  {
    menu.addAction( &selectCurrentArticleAction );
    menu.addAction( ui.definition->pageAction( QWebEnginePage::SelectAll ) );
  }

  map< QAction *, QString > tableOfContents;

  // Add table of contents
  QStringList ids = getArticlesList();

  if ( !menu.isEmpty() && ids.size() )
    menu.addSeparator();

  unsigned refsAdded = 0;
  bool maxDictionaryRefsReached = false;

  for( QStringList::const_iterator i = ids.constBegin(); i != ids.constEnd();
       ++i, ++refsAdded )
  {
    // Find this dictionary

    for( unsigned x = allDictionaries.size(); x--; )
    {
      if ( allDictionaries[ x ]->getId() == i->toUtf8().data() )
      {
        QAction * action = 0;
        if ( refsAdded == cfg.preferences.maxDictionaryRefsInContextMenu )
        {
          // Enough! Or the menu would become too large.
          maxDictionaryRefsAction = new QAction( ".........", &menu );
          action = maxDictionaryRefsAction;
          maxDictionaryRefsReached = true;
        }
        else
        {
          action = new QAction(
                  allDictionaries[ x ]->getIcon(),
                  QString::fromUtf8( allDictionaries[ x ]->getName().c_str() ),
                  &menu );
          // Force icons in menu on all platforms,
          // since without them it will be much harder
          // to find things.
          action->setIconVisibleInMenu( true );
        }
        menu.addAction( action );

        tableOfContents[ action ] = *i;

        break;
      }
    }
    if( maxDictionaryRefsReached )
      break;
  }

  menu.addSeparator();
  menu.addAction( &inspectAction );

  if ( !menu.isEmpty() )
  {
    connect( this, SIGNAL( closePopupMenu() ), &menu, SLOT( close() ) );
    QAction * result = menu.exec( ui.definition->mapToGlobal( pos ) );

    if ( !result )
      return;

    if ( result == followLink )
      openLink( targetUrl, ui.definition->url(), getCurrentArticle(), contexts );
    else
    if ( result == followLinkExternal )
      QDesktopServices::openUrl( targetUrl );
    else
    if ( result == lookupSelection )
      showDefinition( selectedText, getGroup( ui.definition->url() ), getCurrentArticle() );
    else
    if ( result == lookupSelectionGr && groupComboBox )
      showDefinition( selectedText, groupComboBox->getCurrentGroup(), QString() );
    else
    if ( result == addWordToHistoryAction )
      emit forceAddWordToHistory( selectedText );
    if ( result == addHeaderToHistoryAction )
      emit forceAddWordToHistory( ui.definition->title() );
    else
    if( result == sendWordToInputLineAction )
      emit sendWordToInputLine( selectedText );
    else
    if ( !popupView && result == followLinkNewTab )
      emit openLinkInNewTab( targetUrl, ui.definition->url(), getCurrentArticle(), contexts );
    else
    if ( !popupView && result == lookupSelectionNewTab )
      emit showDefinitionInNewTab( selectedText, getGroup( ui.definition->url() ),
                                   getCurrentArticle(), Contexts() );
    else
    if ( !popupView && result == lookupSelectionNewTabGr && groupComboBox )
      emit showDefinitionInNewTab( selectedText, groupComboBox->getCurrentGroup(),
                                   QString(), Contexts() );
    else
    if( result == saveImageAction || result == saveSoundAction )
    {
#if QT_VERSION >= 0x040600
      QUrl url = ( result == saveImageAction ) ? imageUrl : targetUrl;
      QString savePath;
      QString fileName;

      if ( cfg.resourceSavePath.isEmpty() )
        savePath = QDir::homePath();
      else
      {
        savePath = QDir::fromNativeSeparators( cfg.resourceSavePath );
        if ( !QDir( savePath ).exists() )
          savePath = QDir::homePath();
      }

      QString name = Qt4x5::Url::path( url ).section( '/', -1 );

      if ( result == saveSoundAction )
      {
        // Audio data
        if ( name.indexOf( '.' ) < 0 )
          name += ".wav";

        fileName = savePath + "/" + name;
        fileName = QFileDialog::getSaveFileName( parentWidget(), tr( "Save sound" ),
                                                 fileName,
                                                 tr( "Sound files (*.wav *.ogg *.oga *.mp3 *.mp4 *.aac *.flac *.mid *.wv *.ape);;All files (*.*)" ) );
      }
      else
      {
        // Image data

        // Check for babylon image name
        if ( name[ 0 ] == '\x1E' )
          name.remove( 0, 1 );
        if ( name.length() && name[ name.length() - 1 ] == '\x1F' )
          name.chop( 1 );

        fileName = savePath + "/" + name;
        fileName = QFileDialog::getSaveFileName( parentWidget(), tr( "Save image" ),
                                                 fileName,
                                                 tr( "Image files (*.bmp *.jpg *.png *.tif);;All files (*.*)" ) );
      }

      if ( !fileName.isEmpty() )
      {
        QFileInfo fileInfo( fileName );
        emit storeResourceSavePath( QDir::toNativeSeparators( fileInfo.absoluteDir().absolutePath() ) );
        saveResource( url, ui.definition->url(), fileName );
      }
#endif
    }
    else
    {
      if ( !popupView && result == maxDictionaryRefsAction )
        emit showDictsPane();

      // Match against table of contents
      QString id = tableOfContents[ result ];

      if ( id.size() )
        setCurrentArticle( scrollToFromDictionaryId( id ), true );
    }
  }

  qDebug( "url = %s\n", targetUrl.toString().toLocal8Bit().data() );
  qDebug( "title = %s\n", r->title().toLocal8Bit().data() );

}

void ArticleView::resourceDownloadFinished()
{
  if ( resourceDownloadRequests.empty() )
    return; // Stray signal

  // Find any finished resources
  for( list< sptr< Dictionary::DataRequest > >::iterator i =
       resourceDownloadRequests.begin(); i != resourceDownloadRequests.end(); )
  {
    if ( (*i)->isFinished() )
    {
      if ( (*i)->dataSize() >= 0 )
      {
        // Ok, got one finished, all others are irrelevant now

        vector< char > const & data = (*i)->getFullData();

        if ( resourceDownloadUrl.scheme() == "gdau" ||
             Dictionary::WebMultimediaDownload::isAudioUrl( resourceDownloadUrl ) )
        {
          // Audio data
          connect( audioPlayer.data(), SIGNAL( error( QString ) ), this, SLOT( audioPlayerError( QString ) ), Qt::UniqueConnection );
          QString errorMessage = audioPlayer->play( data.data(), data.size() );
          if( !errorMessage.isEmpty() )
            QMessageBox::critical( this, "GoldenDict", tr( "Failed to play sound file: %1" ).arg( errorMessage ) );
        }
        else
        {
          // Create a temporary file
          // Remove the ones previously used, if any
          cleanupTemp();
          QString fileName;

          {
            QTemporaryFile tmp(
              QDir::temp().filePath( "XXXXXX-" + resourceDownloadUrl.path().section( '/', -1 ) ), this );

            if ( !tmp.open() || (size_t) tmp.write( &data.front(), data.size() ) != data.size() )
            {
              QMessageBox::critical( this, "GoldenDict", tr( "Failed to create temporary file." ) );
              return;
            }

            tmp.setAutoRemove( false );

            desktopOpenedTempFiles.insert( fileName = tmp.fileName() );
          }

          if ( !QDesktopServices::openUrl( QUrl::fromLocalFile( fileName ) ) )
            QMessageBox::critical( this, "GoldenDict",
                                   tr( "Failed to auto-open resource file, try opening manually: %1." ).arg( fileName ) );
        }

        // Ok, whatever it was, it's finished. Remove this and any other
        // requests and finish.

        resourceDownloadRequests.clear();

        return;
      }
      else
      {
        // This one had no data. Erase it.
        resourceDownloadRequests.erase( i++ );
      }
    }
    else // Unfinished, wait.
      break;
  }

  if ( resourceDownloadRequests.empty() )
  {
    emit statusBarMessage(
          tr( "WARNING: %1" ).arg( tr( "The referenced resource failed to download." ) ),
          10000, QPixmap( ":/icons/error.png" ) );
  }
}

void ArticleView::audioPlayerError( QString const & message )
{
  emit statusBarMessage( tr( "WARNING: Audio Player: %1" ).arg( message ),
                         10000, QPixmap( ":/icons/error.png" ) );
}

void ArticleView::pasteTriggered()
{
  Config::InputPhrase phrase = cfg.preferences.sanitizeInputPhrase( QApplication::clipboard()->text() );

  if ( phrase.isValid() )
  {
    unsigned groupId = getGroup( ui.definition->url() );
    if ( groupId == 0 )
    {
      // We couldn't figure out the group out of the URL,
      // so let's try the currently selected group.
      groupId = groupComboBox->getCurrentGroup();
    }
    showDefinition( phrase, groupId, getCurrentArticle() );
  }
}

void ArticleView::moveOneArticleUp()
{
  QString current = getCurrentArticle();

  if ( current.size() )
  {
    QStringList lst = getArticlesList();

    int idx = lst.indexOf( dictionaryIdFromScrollTo( current ) );

    if ( idx != -1 )
    {
      --idx;

      if ( idx < 0 )
        idx = lst.size() - 1;

      setCurrentArticle( scrollToFromDictionaryId( lst[ idx ] ), true );
    }
  }
}

void ArticleView::moveOneArticleDown()
{
  QString current = getCurrentArticle();

  if ( current.size() )
  {
    QStringList lst = getArticlesList();

    int idx = lst.indexOf( dictionaryIdFromScrollTo( current ) );

    if ( idx != -1 )
    {
      idx = ( idx + 1 ) % lst.size();

      setCurrentArticle( scrollToFromDictionaryId( lst[ idx ] ), true );
    }
  }
}

void ArticleView::openSearch()
{
  if( !isVisible() )
    return;

  if( ftsSearchIsOpened )
    closeSearch();

  if ( !searchIsOpened )
  {
    ui.searchFrame->show();
    ui.searchText->setText( getTitle() );
    searchIsOpened = true;
  }

  ui.searchText->setFocus();
  ui.searchText->selectAll();

  // Clear any current selection
  if ( ui.definition->selectedText().size() )
  {
    ui.definition->page()->
           runJavaScript( "window.getSelection().removeAllRanges();_=0;" );
  }

  if ( ui.searchText->property( "noResults" ).toBool() )
  {
    ui.searchText->setProperty( "noResults", false );

    // Reload stylesheet
    reloadStyleSheet();
  }
}

void ArticleView::on_searchPrevious_clicked()
{
  if ( searchIsOpened )
    performFindOperation( false, true );
}

void ArticleView::on_searchNext_clicked()
{
  if ( searchIsOpened )
    performFindOperation( false, false );
}

void ArticleView::on_searchText_textEdited()
{
  performFindOperation( true, false );
}

void ArticleView::on_searchText_returnPressed()
{
  on_searchNext_clicked();
}

void ArticleView::on_searchCloseButton_clicked()
{
  closeSearch();
}

void ArticleView::on_searchCaseSensitive_clicked()
{
  performFindOperation( false, false, true );
}

void ArticleView::on_highlightAllButton_clicked()
{
  performFindOperation( false, false, true );
}

void ArticleView::onJsActiveArticleChanged(QString const & id)
{
  if ( !isScrollTo( id ) )
    return; // Incorrect id

  emit activeArticleChanged( this, dictionaryIdFromScrollTo( id ) );
}

void ArticleView::doubleClicked( QPoint pos )
{
  // We might want to initiate translation of the selected word

  if ( cfg.preferences.doubleClickTranslates )
  {
    QString selectedText = ui.definition->selectedText();

    // Do some checks to make sure there's a sensible selection indeed
    if ( Folding::applyWhitespaceOnly( gd::toWString( selectedText ) ).size() &&
         selectedText.size() < 60 )
    {
      // Initiate translation
      Qt::KeyboardModifiers kmod = QApplication::keyboardModifiers();
      if (kmod & (Qt::ControlModifier | Qt::ShiftModifier))
      { // open in new tab
        emit showDefinitionInNewTab( selectedText, getGroup( ui.definition->url() ),
                                     getCurrentArticle(), Contexts() );
      }
      else
      {
        QUrl const & ref = ui.definition->url();

        if( Qt4x5::Url::hasQueryItem( ref, "dictionaries" ) )
        {
          QStringList dictsList = Qt4x5::Url::queryItemValue(ref, "dictionaries" )
                                              .split( ",", QString::SkipEmptyParts );
          showDefinition( selectedText, dictsList, QRegExp(), getGroup( ref ), false );
        }
        else
          showDefinition( selectedText, getGroup( ref ), getCurrentArticle() );
      }
    }
  }
}


void ArticleView::performFindOperation( bool restart, bool backwards, bool checkHighlight )
{
  QString text = ui.searchText->text();

  if ( restart || checkHighlight )
  {
    if( restart ) {
      // Anyone knows how we reset the search position?
      // For now we resort to this hack:
      if ( ui.definition->selectedText().size() )
      {
        ui.definition->page()->
               runJavaScript( "window.getSelection().removeAllRanges();_=0;" );
      }
    }

    QWebEnginePage::FindFlags f( 0 );

    if ( ui.searchCaseSensitive->isChecked() )
      f |= QWebEnginePage::FindCaseSensitively;
#if QT_VERSION >= 0x040600  && QT_VERSION <= 0x050600
    f |= QWebEnginePage::HighlightAllOccurrences;
#endif

    ui.definition->findText( "", f );

    if( ui.highlightAllButton->isChecked() )
      ui.definition->findText( text, f );

    if( checkHighlight )
      return;
  }

  QWebEnginePage::FindFlags f( 0 );

  if ( ui.searchCaseSensitive->isChecked() )
    f |= QWebEnginePage::FindCaseSensitively;

  if ( backwards )
    f |= QWebEnginePage::FindBackward;

  bool setMark = text.size() && !findText(text, f);

  if ( ui.searchText->property( "noResults" ).toBool() != setMark )
  {
    ui.searchText->setProperty( "noResults", setMark );

    // Reload stylesheet
    reloadStyleSheet();
  }
}

bool ArticleView::findText(QString& text, const QWebEnginePage::FindFlags& f)
{
  bool r;
  // turn async to sync invoke.
  QSharedPointer<QEventLoop> loop = QSharedPointer<QEventLoop>(new QEventLoop());
  QTimer::singleShot(1000, loop.data(), &QEventLoop::quit);
  ui.definition->findText(text, f, [&](bool result)
                          {
                            if(loop->isRunning()){
                              r = result;
                              loop->quit();
                            } });

  loop->exec();
  return r;
}

void ArticleView::reloadStyleSheet()
{
  for( QWidget * w = parentWidget(); w; w = w->parentWidget() )
  {
    if ( w->styleSheet().size() )
    {
      w->setStyleSheet( w->styleSheet() );
      break;
    }
  }
}


bool ArticleView::closeSearch()
{
  if ( searchIsOpened )
  {
    ui.searchFrame->hide();
    ui.definition->setFocus();
    searchIsOpened = false;

    return true;
  }
  else
  if( ftsSearchIsOpened )
  {
    allMatches.clear();
    uniqueMatches.clear();
    ftsPosition = 0;
    ftsSearchIsOpened = false;

    ui.ftsSearchFrame->hide();
    ui.definition->setFocus();

    QWebEnginePage::FindFlags flags ( 0 );

  #if QT_VERSION >= 0x040600 && QT_VERSION<=0x050500
    flags |= QWebEnginePage::HighlightAllOccurrences;
  #endif

    ui.definition->findText( "", flags );

    return true;
  }
  else
    return false;
}

bool ArticleView::isSearchOpened()
{
  return searchIsOpened;
}

void ArticleView::showEvent( QShowEvent * ev )
{
  QFrame::showEvent( ev );

  if ( !searchIsOpened )
    ui.searchFrame->hide();

  if( !ftsSearchIsOpened )
    ui.ftsSearchFrame->hide();
}

void ArticleView::receiveExpandOptionalParts( bool expand )
{
  if( expandOptionalParts != expand )
  {
    int n = getArticlesList().indexOf( getActiveArticleId() );
    if( n > 0 )
       articleToJump = getCurrentArticle();

    emit setExpandMode( expand );
    expandOptionalParts = expand;
    reload();
  }
}

void ArticleView::switchExpandOptionalParts()
{
  expandOptionalParts = !expandOptionalParts;

  int n = getArticlesList().indexOf( getActiveArticleId() );
  if( n > 0 )
    articleToJump = getCurrentArticle();

  emit setExpandMode( expandOptionalParts );
  reload();
}

void ArticleView::copyAsText()
{
  QString text = ui.definition->selectedText();
  if( !text.isEmpty() )
    QApplication::clipboard()->setText( text );
}

void ArticleView::inspect()
{
  ui.definition->triggerPageAction( QWebEnginePage::InspectElement );
}

void ArticleView::highlightFTSResults()
{
  closeSearch();

  const QUrl & url = ui.definition->url();

  bool ignoreDiacritics = Qt4x5::Url::hasQueryItem( url, "ignore_diacritics" );

  QString regString = Qt4x5::Url::queryItemValue( url, "regexp" );
  if( ignoreDiacritics )
    regString = gd::toQString( Folding::applyDiacriticsOnly( gd::toWString( regString ) ) );
  else
    regString = regString.remove( AccentMarkHandler::accentMark() );

#if QT_VERSION >= QT_VERSION_CHECK( 5, 0, 0 )
  QRegularExpression regexp;
  if( Qt4x5::Url::hasQueryItem( url, "wildcards" ) )
    regexp.setPattern( wildcardsToRegexp( regString ) );
  else
    regexp.setPattern( regString );

  QRegularExpression::PatternOptions patternOptions = QRegularExpression::DotMatchesEverythingOption
                                                      | QRegularExpression::UseUnicodePropertiesOption
                                                      | QRegularExpression::MultilineOption
                                                      | QRegularExpression::InvertedGreedinessOption;
  if( !Qt4x5::Url::hasQueryItem( url, "matchcase" ) )
    patternOptions |= QRegularExpression::CaseInsensitiveOption;
  regexp.setPatternOptions( patternOptions );

  if( regexp.pattern().isEmpty() || !regexp.isValid() )
    return;
#else
  QRegExp regexp( regString,
                  Qt4x5::Url::hasQueryItem( url, "matchcase" ) ? Qt::CaseSensitive : Qt::CaseInsensitive,
                  Qt4x5::Url::hasQueryItem( url, "wildcards" ) ? QRegExp::WildcardUnix : QRegExp::RegExp2 );


  if( regexp.pattern().isEmpty() )
    return;

  regexp.setMinimal( true );
#endif

  sptr< AccentMarkHandler > marksHandler = ignoreDiacritics ?
                                           new DiacriticsHandler : new AccentMarkHandler;

  // Clear any current selection
  if ( ui.definition->selectedText().size() )
  {
    ui.definition->page()->
           runJavaScript( "window.getSelection().removeAllRanges();_=0;" );
  }

  QString pageText = getWebPageTextSync(ui.definition->page());
  marksHandler->setText( pageText );

#if QT_VERSION >= QT_VERSION_CHECK( 5, 0, 0 )
  QRegularExpressionMatchIterator it = regexp.globalMatch( marksHandler->normalizedText() );
  while( it.hasNext() )
  {
    QRegularExpressionMatch match = it.next();

    // Mirror pos and matched length to original string
    int pos = match.capturedStart();
    int spos = marksHandler->mirrorPosition( pos );
    int matched = marksHandler->mirrorPosition( pos + match.capturedLength() ) - spos;

    // Add mark pos (if presented)
    while( spos + matched < pageText.length()
           && pageText[ spos + matched ].category() == QChar::Mark_NonSpacing )
      matched++;

    if( matched > FTS::MaxMatchLengthForHighlightResults )
    {
      gdWarning( "ArticleView::highlightFTSResults(): Too long match - skipped (matched length %i, allowed %i)",
                 match.capturedLength(), FTS::MaxMatchLengthForHighlightResults );
    }
    else
      allMatches.append( pageText.mid( spos, matched ) );
  }
#else
  int pos = 0;

  while( pos >= 0 )
  {
    pos = regexp.indexIn( marksHandler->normalizedText(), pos );
    if( pos >= 0 )
    {
      // Mirror pos and matched length to original string
      int spos = marksHandler->mirrorPosition( pos );
      int matched = marksHandler->mirrorPosition( pos + regexp.matchedLength() ) - spos;

      // Add mark pos (if presented)
      while( spos + matched < pageText.length()
             && pageText[ spos + matched ].category() == QChar::Mark_NonSpacing )
        matched++;

      if( matched > FTS::MaxMatchLengthForHighlightResults )
      {
        gdWarning( "ArticleView::highlightFTSResults(): Too long match - skipped (matched length %i, allowed %i)",
                   regexp.matchedLength(), FTS::MaxMatchLengthForHighlightResults );
      }
      else
        allMatches.append( pageText.mid( spos, matched ) );

      pos += regexp.matchedLength();
    }
  }
#endif

  ftsSearchMatchCase = Qt4x5::Url::hasQueryItem( url, "matchcase" );

  QWebEnginePage::FindFlags flags ( 0 );

  if( ftsSearchMatchCase )
    flags |= QWebEnginePage::FindCaseSensitively;

#if QT_VERSION >= 0x040600
 // flags |= QWebEnginePage::HighlightAllOccurrences;

  for( int x = 0; x < allMatches.size(); x++ )
    ui.definition->findText( allMatches.at( x ), flags );

 // flags &= ~QWebEnginePage::HighlightAllOccurrences;
#endif

  if( !allMatches.isEmpty() )
  {
      ui.definition->findText( allMatches.at( 0 ), flags );
    //if( ui.definition->findText( allMatches.at( 0 ), flags ) )
    {
        ui.definition->page()->
               runJavaScript( QString( "%1=window.getSelection().getRangeAt(0);_=0;" )
                                   .arg( rangeVarName ) );
    }
  }

  ui.ftsSearchFrame->show();
  ui.ftsSearchPrevious->setEnabled( false );
  ui.ftsSearchNext->setEnabled( allMatches.size()>1 );

  ftsSearchIsOpened = true;
}

QString ArticleView::getWebPageTextSync(QWebEnginePage * page){
    QString planText;
    QSharedPointer<QEventLoop> loop = QSharedPointer<QEventLoop>(new QEventLoop());
    QTimer::singleShot(1000, loop.data(), &QEventLoop::quit);
    page->toPlainText([&](const QString &result)
                      {
                        if(loop->isRunning()){
                          planText = result;
                          loop->quit();
                        } });
    loop->exec();
    return planText;
}

void ArticleView::setActiveDictIds(ActiveDictIds ad) {
  // ignore all other signals.

  if (ad.word == currentWord) {
    currentActiveDictIds << ad.dictIds;
    currentActiveDictIds.removeDuplicates();
    emit updateFoundInDictsList();
    qDebug() << "receive dicts:"<<ad.word<<":" << ad.dictIds;
  }
}

//todo ,futher refinement?
void ArticleView::performFtsFindOperation( bool backwards )
{
  if( !ftsSearchIsOpened )
    return;

  if( allMatches.isEmpty() )
  {
    ui.ftsSearchNext->setEnabled( false );
    ui.ftsSearchPrevious->setEnabled( false );
    return;
  }

  QWebEnginePage::FindFlags flags( 0 );

  if( ftsSearchMatchCase )
    flags |= QWebEnginePage::FindCaseSensitively;


  // Restore saved highlighted selection
  ui.definition->page()->
         runJavaScript( QString( "var sel=window.getSelection();sel.removeAllRanges();sel.addRange(%1);_=0;" )
                             .arg( rangeVarName ) );

  if (backwards) {
      if (ftsPosition > 0) {
          ftsPosition -= 1;
      }

      ui.definition->findText(allMatches.at(ftsPosition),
                              flags | QWebEnginePage::FindBackward,
                              [this](bool res) {
                                  ui.ftsSearchPrevious->setEnabled(res);
                                  if (!ui.ftsSearchNext->isEnabled())
                                      ui.ftsSearchNext->setEnabled(res);
                              });
  } else {
      if (ftsPosition < allMatches.size() - 1) {
          ftsPosition += 1;
      }

      ui.definition->findText(allMatches.at(ftsPosition), flags, [this](bool res) {
          ui.ftsSearchNext->setEnabled(res);
          if (!ui.ftsSearchPrevious->isEnabled())
              ui.ftsSearchPrevious->setEnabled(res);
      });
  }

  // Store new highlighted selection
  ui.definition->page()->
         runJavaScript( QString( "%1=window.getSelection().getRangeAt(0);_=0;" )
                             .arg( rangeVarName ) );
}

void ArticleView::on_ftsSearchPrevious_clicked()
{
  performFtsFindOperation( true );
}

void ArticleView::on_ftsSearchNext_clicked()
{
  performFtsFindOperation( false );
}

#ifdef Q_OS_WIN32

void ArticleView::readTag( const QString & from, QString & to, int & count )
{
    QChar ch, prev_ch;
    bool inQuote = false, inDoublequote = false;

    to.append( ch = prev_ch = from[ count++ ] );
    while( count < from.size() )
    {
        ch = from[ count ];
        if( ch == '>' && !( inQuote || inDoublequote ) )
        {
            to.append( ch );
            break;
        }
        if( ch == '\'' )
            inQuote = !inQuote;
        if( ch == '\"' )
            inDoublequote = !inDoublequote;
        to.append( prev_ch = ch );
        count++;
    }
}

QString ArticleView::insertSpans( QString const & html )
{
    QChar ch;
    QString newContent;
    bool inSpan = false, escaped = false;

    /// Enclose every word in string (exclude tags) with <span></span>

    for( int i = 0; i < html.size(); i++ )
    {
        ch = html[ i ];
        if( ch == '&' )
        {
            escaped = true;
            if( inSpan )
            {
                newContent.append( "</span>" );
                inSpan = false;
            }
            newContent.append( ch );
            continue;
        }

        if( ch == '<' ) // Skip tag
        {
            escaped = false;
            if( inSpan )
            {
                newContent.append( "</span>" );
                inSpan = false;
            }
            readTag( html, newContent, i );
            continue;
        }

        if( escaped )
        {
            if( ch == ';' )
                escaped = false;
            newContent.append( ch );
            continue;
        }

        if( !inSpan && ( ch.isLetterOrNumber() || ch.isLowSurrogate() ) )
        {
            newContent.append( "<span>");
            inSpan = true;
        }

        if( inSpan && !( ch.isLetterOrNumber() || ch.isLowSurrogate() ) )
        {
            newContent.append( "</span>");
            inSpan = false;
        }

        if( ch.isLowSurrogate() )
        {
            newContent.append( ch );
            ch = html[ ++i ];
        }

        newContent.append( ch );
        if( ch == '-' && !( html[ i + 1 ] == ' ' || ( i > 0 && html[ i - 1 ] == ' ' ) ) )
            newContent.append( "<span style=\"font-size:0pt\"> </span>" );
    }
    if( inSpan )
        newContent.append( "</span>" );
    return newContent;
}

QString ArticleView::checkElement( QWebEnginePage & page, QPoint const & pt )
{
  return runJavaScriptSync(&page, QString(
                                     " var a= document.elementFromPoint(%1,%2);"
                                     "var nodename=a.nodeName.toLowerCase();"
                                     "if(nodename==\"body\"||nodename==\"html\"||nodename==\"head\")"
                                     "{"
                                     "   return '';"
                                     "}"
                                     "return a.textContent;")
                                     .arg(pt.x())
                                     .arg(pt.y()));
}

QString ArticleView::wordAtPoint( int x, int y )
{
  QString word;

  if( popupView )
    return word;

  QPoint pos = mapFromGlobal( QPoint( x, y ) );
  //todo
  QWebEnginePage *frame = ui.definition->page();
  if( !frame )
    return word;

  QPointF scrollPoint=frame->scrollPosition();

  QPoint posWithScroll = pos + QPoint((int)scrollPoint.x(),(int)scrollPoint.y());

  /// Find target HTML element
  QString nodeValue = runJavaScriptSync(frame, QString(
                                                   "var a= document.elementFromPoint(%1,%2);"
                                                   "var nodename=a.nodeName.toLowerCase();"
                                                   "if(nodename==\"body\"||nodename==\"html\"||nodename==\"head\")"
                                                   "{"
                                                   "   return '';"
                                                   "}"
                                                   "return a.textContent;")
                                                   .arg(posWithScroll.x())
                                                   .arg(posWithScroll.y()));
  return nodeValue;
}

#endif

ResourceToSaveHandler::ResourceToSaveHandler(ArticleView * view, QString const & fileName ) :
  QObject( view ),
  fileName( fileName ),
  alreadyDone( false )
{
  connect( this, SIGNAL( statusBarMessage( QString, int, QPixmap ) ),
           view, SIGNAL( statusBarMessage( QString, int, QPixmap ) ) );
}

void ResourceToSaveHandler::addRequest( sptr<Dictionary::DataRequest> req )
{
  if( !alreadyDone )
  {
    downloadRequests.push_back( req );

    connect( req.get(), SIGNAL( finished() ),
             this, SLOT( downloadFinished() ) );
  }
}

void ResourceToSaveHandler::downloadFinished()
{
  if ( downloadRequests.empty() )
    return; // Stray signal

  // Find any finished resources
  for( list< sptr< Dictionary::DataRequest > >::iterator i =
       downloadRequests.begin(); i != downloadRequests.end(); )
  {
    if ( (*i)->isFinished() )
    {
      if ( (*i)->dataSize() >= 0 && !alreadyDone )
      {
        QByteArray resourceData;
        vector< char > const & data = (*i)->getFullData();
        resourceData = QByteArray( data.data(), data.size() );

        // Write data to file

        if ( !fileName.isEmpty() )
        {
          QFileInfo fileInfo( fileName );
          QDir().mkpath( fileInfo.absoluteDir().absolutePath() );

          QFile file( fileName );
          if ( file.open( QFile::WriteOnly ) )
          {
            file.write( resourceData.data(), resourceData.size() );
            file.close();
          }

          if ( file.error() )
          {
            emit statusBarMessage(
                  tr( "ERROR: %1" ).arg( tr( "Resource saving error: " ) + file.errorString() ),
                  10000, QPixmap( ":/icons/error.png" ) );
          }
        }
        alreadyDone = true;

        // Clear other requests

        downloadRequests.clear();
        break;
      }
      else
      {
        // This one had no data. Erase it.
        downloadRequests.erase( i++ );
      }
    }
    else // Unfinished, wait.
      break;
  }

  if ( downloadRequests.empty() )
  {
    if( !alreadyDone )
    {
      emit statusBarMessage(
            tr( "WARNING: %1" ).arg( tr( "The referenced resource failed to download." ) ),
            10000, QPixmap( ":/icons/error.png" ) );
    }
    emit done();
    deleteLater();
  }
}
