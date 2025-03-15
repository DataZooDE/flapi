#pragma once

#include <crow.h>
#include "config_manager.hpp"

namespace flapi {

const std::string openapi_page_template = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Swagger UI for {{projectName}}</title>
    <link rel="stylesheet" type="text/css" href="https://unpkg.com/swagger-ui-dist@5.18.2/swagger-ui.css">
    <style>
        body {
            margin: 0;
            padding: 0;
        }
        #swagger-ui {
            margin: 0 auto;
            max-width: 1460px;
            padding: 20px;
        }
    </style>
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://unpkg.com/swagger-ui-dist@5.18.2/swagger-ui-bundle.js"></script>
    <script src="https://unpkg.com/swagger-ui-dist@5.18.2/swagger-ui-standalone-preset.js"></script>
    <script>
        window.onload = function() {
            const currentOrigin = window.location.origin;
            const apiDocUrl = `${currentOrigin}{{{apiDocPath}}}`;

            const ui = SwaggerUIBundle({
                urls: [{url: apiDocUrl, name: "{{projectName}} API"}],
                dom_id: '#swagger-ui',
                presets: [
                    SwaggerUIBundle.presets.apis,
                    SwaggerUIStandalonePreset
                ],
                plugins: [
                    SwaggerUIBundle.plugins.DownloadUrl
                ],
                onComplete: function() {
                    // This runs after the spec is loaded and UI is rendered
                    // Get the spec from the UI
                    const spec = ui.getState().get("spec").toJS().json;
                    
                    // Update servers if they exist
                    if (spec.servers) {
                        // Create a copy of the spec
                        const updatedSpec = {...spec};
                        // Modify servers
                        updatedSpec.servers = [{ url: currentOrigin }];
                        // Update the UI
                        ui.specActions.updateJsonSpec(updatedSpec);
                    }
                },
                deepLinking: true,
                layout: "StandaloneLayout",
                displayRequestDuration: true,
                persistAuthorization: true,
                showExtensions: true,
                showCommonExtensions: true,
                requestSnippetsEnabled: true,
                syntaxHighlight: {
                    activate: true,
                    theme: 'tomorrow-night'
                }
            });
            window.ui = ui;
        };
    </script>
</body>
</html>
)";

crow::mustache::rendered_template generateOpenAPIPage(std::shared_ptr<ConfigManager> config_manager) {
    crow::mustache::context ctx;
    ctx["projectName"] = config_manager->getProjectName();
    ctx["apiDocPath"] = "/doc.yaml";

    auto compiled_template = crow::mustache::compile(openapi_page_template);
    
    return compiled_template.render(ctx);
}

} // namespace flapi