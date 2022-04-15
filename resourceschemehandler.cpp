#include "resourceschemehandler.h"

ResourceSchemeHandler::ResourceSchemeHandler(ArticleNetworkAccessManager& articleNetMgr):mManager(articleNetMgr){

}
void ResourceSchemeHandler::requestStarted(QWebEngineUrlRequestJob *requestJob)
{
  QUrl url = requestJob->requestUrl();

  QNetworkRequest request;
  request.setUrl( url );
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    // cache management
  auto cache = mManager.getCachedReply( url.url() );
  if( cache )
  {
//    sptr< Dictionary::DataRequestInstant > ico = new Dictionary::DataRequestInstant( true );
//    if( cache->data.size() > 0 )
//    {
//      ico->getData().resize( cache->data.size() );
//      memcpy( &( ico->getData().front() ), cache->data.data(), cache->data.size() );
//    }
//    // get the cache will remove the cache ,set the cache again to enable future access.
//    // setCachedReply(url.url(),reply);
//    QNetworkReply * reply1 = new ArticleResourceReply( this, request, ico ,nullptr);
    QMimeType mineType     = db.mimeTypeForUrl( url );
    QString contentType    = mineType.name();
    // Reply segment
    QBuffer * buffer         = new QBuffer(this);
    
    buffer->setData(&cache->data.front(),cache->data.size());
    buffer->open( QBuffer::ReadOnly );
    requestJob->reply( contentType.toLatin1(), buffer );
    connect( requestJob, &QObject::destroyed, buffer, &QObject::deleteLater );
    //buffer.close();
    return;
  }

  QNetworkReply * reply = this->mManager.createRequest( QNetworkAccessManager::GetOperation, request );

  connect( reply,
           &QNetworkReply::finished,
           requestJob,
           [ = ]()
           {
             ArticleResourceReply * const dr = qobject_cast< ArticleResourceReply * >( reply );
             if( dr )
             {
               if( dr->dataSize() > 0 )
               {
                 CacheReply * cr = new CacheReply();

                 vector< char > data = dr->getAllData();
                 cr->data.assign( data.begin(), data.end() );
                 mManager.setCachedReply( url.url(), cr );
               }
             }

             if( reply->error() == QNetworkReply::ContentNotFoundError )
             {
               requestJob->fail( QWebEngineUrlRequestJob::UrlNotFound );
               return;
             }
             if( reply->error() != QNetworkReply::NoError )
             {
               qDebug() << "resource handler failed:" << reply->error() << ":" << reply->request().url();
               requestJob->fail( QWebEngineUrlRequestJob::RequestFailed );
               return;
             }
             QMimeType mineType  = db.mimeTypeForUrl( url );
             QString contentType = mineType.name();
             // Reply segment
             requestJob->reply( contentType.toLatin1(), reply );
           } );

  connect( requestJob, &QObject::destroyed, reply, &QObject::deleteLater );
}
