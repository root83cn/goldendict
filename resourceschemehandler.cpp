#include "resourceschemehandler.h"

ResourceSchemeHandler::ResourceSchemeHandler(ArticleNetworkAccessManager& articleNetMgr):mManager(articleNetMgr){

}
void ResourceSchemeHandler::requestStarted(QWebEngineUrlRequestJob *requestJob)
{
  QUrl url = requestJob->requestUrl();

  QNetworkRequest request;
  request.setUrl( url );
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
  QNetworkReply * reply = this->mManager.createRequest( QNetworkAccessManager::GetOperation, request );

  connect( reply,
           &QNetworkReply::finished,
           requestJob,
           [ = ]()
           {
             ArticleResourceReply * const dr = qobject_cast< ArticleResourceReply * >( reply );
             if( dr )
             {
               CacheReply * cr     = new CacheReply();
               vector< char > data = dr->getAllData();
               if( dr->dataSize() > 0 )
               {
                 cr->data.assign( data.begin(), data.end() );
               }
               mManager.setCachedReply( url.url(), cr );
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
