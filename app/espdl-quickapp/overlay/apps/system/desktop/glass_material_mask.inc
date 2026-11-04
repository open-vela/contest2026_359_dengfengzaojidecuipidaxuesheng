/* GUI-owned transient A8 decode; pixel source descriptors never own RAM. */
struct material_mask_decode {lv_draw_buf_t *pixels;};
static lv_image_decoder_t *material_mask_decoder;
static const struct material_mask_source *material_mask_get(lv_image_decoder_dsc_t *dsc)
{
 if(dsc->src_type!=LV_IMAGE_SRC_VARIABLE||!dsc->src)return NULL;
 uintptr_t p=(uintptr_t)dsc->src,first=(uintptr_t)&icon_mask_images[0],last=(uintptr_t)(icon_mask_images+sizeof(icon_mask_images)/sizeof(icon_mask_images[0]));
 if(p<first||p>=last||(p-first)%sizeof(icon_mask_images[0]))return NULL;
 return (const struct material_mask_source *)dsc->src;
}
static lv_result_t material_mask_info(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc,lv_image_header_t *header)
{LV_UNUSED(decoder);const struct material_mask_source *s=material_mask_get(dsc);if(!s)return LV_RESULT_INVALID;*header=s->image.header;header->cf=LV_COLOR_FORMAT_A8;return LV_RESULT_OK;}
static lv_result_t material_mask_open(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{
 LV_UNUSED(decoder);const struct material_mask_source *s=material_mask_get(dsc);if(!s)return LV_RESULT_INVALID;
 struct material_mask_decode *context=calloc(1,sizeof(*context));if(!context)return LV_RESULT_INVALID;
 context->pixels=lv_draw_buf_create(s->image.header.w,s->image.header.h,LV_COLOR_FORMAT_A8,LV_STRIDE_AUTO);
 if(!context->pixels){free(context);return LV_RESULT_INVALID;}
 lv_draw_buf_t *b=context->pixels;memset(b->data,0,b->data_size);bool ok=true;
 void *raw=gmat_acquire(GMAT_ICONS_MASKS_BIN,s->offset,s->bytes);ok=raw!=NULL;
 if(raw){for(unsigned y=0;y<s->image.header.h;y++)memcpy(b->data+(size_t)y*b->header.stride,(const unsigned char *)raw+(size_t)y*s->image.header.stride,s->image.header.w);gmat_release(raw);}
 /* Missing/corrupt card yields an empty decoration; text controls remain usable. */
 if(!ok)memset(b->data,0,b->data_size);
 dsc->args.no_cache=true;dsc->user_data=context;dsc->decoded=b;return LV_RESULT_OK;
}
static void material_mask_close(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *dsc)
{LV_UNUSED(decoder);struct material_mask_decode *c=dsc->user_data;if(c){lv_draw_buf_destroy(c->pixels);free(c);}dsc->user_data=NULL;dsc->decoded=NULL;}
static void material_mask_register(void)
{if(material_mask_decoder)return;material_mask_decoder=lv_image_decoder_create();if(!material_mask_decoder)return;
 lv_image_decoder_set_info_cb(material_mask_decoder,material_mask_info);lv_image_decoder_set_open_cb(material_mask_decoder,material_mask_open);lv_image_decoder_set_close_cb(material_mask_decoder,material_mask_close);}
